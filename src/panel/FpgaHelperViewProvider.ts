import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import * as os from 'os';
import { SerialPort } from 'serialport';
import { usb } from 'usb';
import { buildProject, createProject, hasApioProject, isApioInstalled, isEmptyDir, ProcessError, graphicalBuild, simulateProject, testProject } from '../build/apioEnv';
import { uploadProject } from '../build/flash';
import { runInTerminal } from '../build/taskTerminal';
import { listExamples, copyExampleProject } from '../build/examples';

const IGNORED_PORT_PATTERNS = ['debug-console', 'Bluetooth-Incoming-Port'];
const NO_BOARD_SIGNATURES = ['no cable detected', 'found 0 devices', 'no device found'];

async function listPorts(): Promise<string[]> {
  const ports = await SerialPort.list();
  return ports
    .map(p => p.path)
    .filter(p => !IGNORED_PORT_PATTERNS.some(pattern => p.includes(pattern)));
}

function looksLikeNoBoardFound(err: unknown): boolean {
  if (!(err instanceof ProcessError)) {
    return false;
  }
  const lower = err.output.toLowerCase();
  return NO_BOARD_SIGNATURES.some(sig => lower.includes(sig));
}

// The generic "command exited with code N" in err.message is useless in the
// sidebar — pull the actual tool's error line out of the captured output
// instead (last line mentioning "error", since that's usually the real cause
// rather than an earlier warning).
// apio/scons wrap the real tool error in their own summary lines ("scons:
// *** [...] Error 1", "===== [ERROR] Took Ns ====") — those match /error/i
// too and sit AFTER the actual cause, so filter them out and take the
// first remaining match rather than the last.
const ERROR_NOISE_PATTERNS = [/^scons:\s*\*\*\*/i, /took .* seconds/i, /^=+$/, /^-+$/];

function extractErrorSnippet(err: unknown): string {
  if (err instanceof ProcessError) {
    const lines = err.output.split('\n').map(l => l.trim()).filter(Boolean)
      .filter(l => !ERROR_NOISE_PATTERNS.some(re => re.test(l)));
    const errorLine = lines.find(l => /error/i.test(l));
    if (errorLine) {
      return errorLine.slice(0, 200);
    }
  }
  return err instanceof Error ? err.message : String(err);
}

export class FpgaHelperViewProvider implements vscode.WebviewViewProvider {

  private _view?: vscode.WebviewView;
  private diagnostics = vscode.window.createOutputChannel('FPGA Helper: Ports');
  private fastPollTimer?: ReturnType<typeof setInterval>;
  private fastPollStopTimer?: ReturnType<typeof setTimeout>;
  private lastPorts: string[] = [];

  constructor(private readonly context: vscode.ExtensionContext) {
    // The CDC serial device node's appearance lags the raw USB attach event
    // by a variable, OS-driver-dependent amount (observed ~6s on some
    // setups) — a fixed delay can't be trusted, so poll fast for a bounded
    // window instead of guessing a single "long enough" wait.
    const onUsbChange = (event: 'connect' | 'disconnect') => {
      this.diagnostics.appendLine(`[usb] ${event} event received`);
      this.startFastPoll();
    };
    const onConnect = () => onUsbChange('connect');
    const onDisconnect = () => onUsbChange('disconnect');
    usb.addEventListener('connect', onConnect);
    usb.addEventListener('disconnect', onDisconnect);
    context.subscriptions.push({
      dispose: () => {
        usb.removeEventListener('connect', onConnect);
        usb.removeEventListener('disconnect', onDisconnect);
        this.stopFastPoll();
      }
    });
  }

  private startFastPoll(): void {
    if (!this.fastPollTimer) {
      this.fastPollTimer = setInterval(() => this.refreshPorts(), 500);
      this._view?.webview.postMessage({ command: 'scanningState', active: true });
    }
    if (this.fastPollStopTimer) {
      clearTimeout(this.fastPollStopTimer);
    }
    this.fastPollStopTimer = setTimeout(() => this.stopFastPoll(), 10000);
  }

  private stopFastPoll(): void {
    if (this.fastPollTimer) {
      clearInterval(this.fastPollTimer);
      this.fastPollTimer = undefined;
      this._view?.webview.postMessage({ command: 'scanningState', active: false });
    }
    if (this.fastPollStopTimer) {
      clearTimeout(this.fastPollStopTimer);
      this.fastPollStopTimer = undefined;
    }
  }

  resolveWebviewView(webviewView: vscode.WebviewView): void {
    this._view = webviewView;

    webviewView.webview.options = { enableScripts: true };
    webviewView.webview.html = this.getHtml();

    webviewView.onDidChangeVisibility(() => {
      if (webviewView.visible) {
        this.refreshPorts();
        this.reportProjectContext();
      }
    });

    // A fresh webview always starts with an empty <select>, so force at
    // least one populatePorts send even if the port list is unchanged from
    // what a previous (now-discarded) webview instance last saw.
    this.lastPorts = [];
    this.refreshPorts();
    this.reportProjectContext();

    webviewView.webview.onDidReceiveMessage((message) => {
      switch (message.command) {

        case 'getPorts':
          this.refreshPorts();
          break;

        case 'openProject':
          this.openProject();
          break;

        case 'generateHere':
          this.generateHere();
          break;

        case 'createProjectBrowse':
          this.createProjectBrowse();
          break;

        case 'createProjectFromExample':
          this.createProjectFromExample();
          break;

        case 'build':
          this.build();
          break;

        case 'upload':
          this.upload();
          break;

        case 'graphicalBuild':
          this.graphicalBuild();
          break;

        case 'simulate':
          this.simulate();
          break;

        case 'test':
          this.test();
          break;

        case 'openWebsite':
          vscode.env.openExternal(vscode.Uri.parse('https://soldered.com'));
          break;

        case 'openGithub':
          vscode.env.openExternal(vscode.Uri.parse('https://github.com/SolderedElectronics'));
          break;
      }
    });
  }

  private async refreshPorts(): Promise<void> {
    try {
      const ports = await listPorts();
      this.diagnostics.appendLine(`[scan] ${ports.length ? ports.join(', ') : '(none found)'}`);

      const changed = ports.length !== this.lastPorts.length
        || ports.some(p => !this.lastPorts.includes(p));
      this.lastPorts = ports;

      if (changed) {
        this._view?.webview.postMessage({ command: 'populatePorts', ports });
        // Something actually happened — no need to keep hammering the fast poll.
        if (this.fastPollTimer) {
          this.stopFastPoll();
        }
      }
    } catch (err: any) {
      this.diagnostics.appendLine(`[ERROR] Port scan failed: ${err?.message || err}`);
      this.diagnostics.show(true);
    }
  }

  private reportTaskStatus(task: string, state: 'running' | 'success' | 'error', message?: string): void {
    this._view?.webview.postMessage({ command: 'taskStatus', task, state, message });
  }

  private getWorkspaceDir(): string | undefined {
    return vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  }

  private reportProjectContext(): void {
    const dir = this.getWorkspaceDir();
    const ready = dir ? hasApioProject(dir) : false;
    this._view?.webview.postMessage({
      command: 'projectContext',
      hasWorkspace: !!dir,
      empty: dir ? isEmptyDir(dir) : false,
      ready,
    });
    if (ready) {
      this.suggestVerilogExtensionOnce();
    }
  }

  private suggestVerilogExtensionOnce(): void {
    if (this.context.globalState.get('verilogPromptShown')) {
      return;
    }
    const hasVerilogSupport = vscode.extensions.all.some(ext => {
      const languages = ext.packageJSON?.contributes?.languages;
      return Array.isArray(languages) && languages.some((l: any) => l.id === 'verilog');
    });
    if (hasVerilogSupport) {
      return;
    }

    this.context.globalState.update('verilogPromptShown', true);
    vscode.window.showInformationMessage(
      'No Verilog syntax highlighting extension detected — want to find one in the marketplace?',
      'Search Extensions'
    ).then((choice) => {
      if (choice === 'Search Extensions') {
        vscode.commands.executeCommand('workbench.extensions.search', 'verilog');
      }
    });
  }

  private async confirmFirstRunIfNeeded(): Promise<boolean> {
    if (isApioInstalled(this.context)) {
      return true;
    }
    const choice = await vscode.window.showInformationMessage(
      'First-time setup will download the FPGA toolchain (roughly 1-2GB) and can take a few minutes. Continue?',
      'Continue'
    );
    return choice === 'Continue';
  }

  private openProject(): void {
    vscode.window.showOpenDialog({
      canSelectFiles: false,
      canSelectFolders: true,
      canSelectMany: false,
      title: 'Open Soldered FPGA Mini1 Project'
    }).then((picked) => {
      if (picked && picked.length > 0) {
        vscode.commands.executeCommand('vscode.openFolder', picked[0]);
      }
    });
  }

  private runCreateProject(targetDir: string, onSuccess: () => void): void {
    this.reportTaskStatus('createProject', 'running');
    runInTerminal(
      'FPGA Helper: Create Project',
      (out) => createProject(this.context, targetDir, out),
      (err) => {
        if (err) {
          const snippet = extractErrorSnippet(err);
          this.reportTaskStatus('createProject', 'error', snippet);
          vscode.window.showErrorMessage(`Couldn't create the project: ${snippet}`);
          return;
        }
        this.reportTaskStatus('createProject', 'success');
        onSuccess();
      }
    );
  }

  private async generateHere(): Promise<void> {
    const dir = this.getWorkspaceDir();
    if (!dir) {
      vscode.window.showErrorMessage('Open an empty folder first (File > Open Folder).');
      return;
    }
    if (!isEmptyDir(dir)) {
      vscode.window.showErrorMessage('This folder is no longer empty — use "Create New Project" to pick a different location.');
      this.reportProjectContext();
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.runCreateProject(dir, () => {
      this.reportProjectContext();
      vscode.window.showInformationMessage('Project ready! Click Build & Upload to program your board.');
    });
  }

  private async createProjectBrowse(): Promise<void> {
    const workspaceDir = this.getWorkspaceDir();
    const defaultParent = workspaceDir ? path.dirname(workspaceDir) : os.homedir();

    const picked = await vscode.window.showSaveDialog({
      title: 'Create Soldered FPGA Mini1 Project',
      saveLabel: 'Create Project',
      defaultUri: vscode.Uri.file(path.join(defaultParent, 'soldered-fpga-mini1-project'))
    });
    if (!picked) {
      return;
    }

    const targetDir = picked.fsPath;
    if (fs.existsSync(targetDir) && !isEmptyDir(targetDir)) {
      vscode.window.showErrorMessage('That folder already has files in it. Pick an empty or new folder.');
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }
    fs.mkdirSync(targetDir, { recursive: true });

    this.runCreateProject(targetDir, () => {
      vscode.window.showInformationMessage(
        `Project created at ${targetDir} — opening it now. Once it's open, click Build & Upload.`
      );
      vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(targetDir));
    });
  }

  private async createProjectFromExample(): Promise<void> {
    const examples = listExamples();
    if (examples.length === 0) {
      vscode.window.showErrorMessage('No example projects found on disk.');
      return;
    }

    const picked = await vscode.window.showQuickPick(
      examples.map(example => ({ label: example.id, example })),
      { title: 'Choose an Example Project' }
    );
    if (!picked) {
      return;
    }

    const workspaceDir = this.getWorkspaceDir();
    const defaultParent = workspaceDir ? path.dirname(workspaceDir) : os.homedir();

    const target = await vscode.window.showSaveDialog({
      title: 'Create Project From Example',
      saveLabel: 'Create Project',
      defaultUri: vscode.Uri.file(path.join(defaultParent, picked.example.id))
    });
    if (!target) {
      return;
    }

    const targetDir = target.fsPath;
    if (fs.existsSync(targetDir) && !isEmptyDir(targetDir)) {
      vscode.window.showErrorMessage('That folder already has files in it. Pick an empty or new folder.');
      return;
    }
    fs.mkdirSync(targetDir, { recursive: true });

    this.reportTaskStatus('createProject', 'running');
    try {
      copyExampleProject(picked.example.dir, targetDir);
      this.reportTaskStatus('createProject', 'success');
      vscode.window.showInformationMessage(
        `Project created at ${targetDir} — opening it now. Once it's open, click Build & Upload.`
      );
      vscode.commands.executeCommand('vscode.openFolder', vscode.Uri.file(targetDir));
    } catch (err: any) {
      const snippet = extractErrorSnippet(err);
      this.reportTaskStatus('createProject', 'error', snippet);
      vscode.window.showErrorMessage(`Couldn't create the project: ${snippet}`);
    }
  }

  private async offerCreateProjectIfMissing(projectDir: string): Promise<boolean> {
    if (hasApioProject(projectDir)) {
      return true;
    }
    const choice = await vscode.window.showWarningMessage(
      "This folder isn't set up as an apio project yet (no apio.ini found).",
      isEmptyDir(projectDir) ? 'Generate Project Files Here' : 'Create New Project'
    );
    if (choice === 'Generate Project Files Here') {
      this.generateHere();
    } else if (choice === 'Create New Project') {
      this.createProjectBrowse();
    }
    return false;
  }

  private async build(): Promise<void> {
    const projectDir = this.getWorkspaceDir();
    if (!projectDir) {
      vscode.window.showErrorMessage('Open the folder containing your apio project (apio.ini) before building.');
      return;
    }
    if (!await this.offerCreateProjectIfMissing(projectDir)) {
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.reportTaskStatus('build', 'running');
    runInTerminal('FPGA Helper: Build', (out) => buildProject(this.context, projectDir, out), (err) => {
      this.reportTaskStatus('build', err ? 'error' : 'success', err ? extractErrorSnippet(err) : undefined);
    });
  }

  private async graphicalBuild(): Promise<void> {
    const projectDir = this.getWorkspaceDir();
    if (!projectDir) {
      vscode.window.showErrorMessage('Open the folder containing your apio project (apio.ini) before viewing it.');
      return;
    }
    if (!await this.offerCreateProjectIfMissing(projectDir)) {
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.reportTaskStatus('graphicalBuild', 'running');
    runInTerminal('FPGA Helper: Graphical Build', (out) => graphicalBuild(this.context, projectDir, out), (err) => {
      this.reportTaskStatus('graphicalBuild', err ? 'error' : 'success', err ? extractErrorSnippet(err) : undefined);
    });
  }

  private async upload(): Promise<void> {
    const projectDir = this.getWorkspaceDir();
    if (!projectDir) {
      vscode.window.showErrorMessage('Open the folder containing your apio project (apio.ini) before uploading.');
      return;
    }
    if (!await this.offerCreateProjectIfMissing(projectDir)) {
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.reportTaskStatus('upload', 'running');
    runInTerminal('FPGA Helper: Upload', (out) => uploadProject(this.context, projectDir, out), (err) => {
      if (err && looksLikeNoBoardFound(err)) {
        this.reportTaskStatus('upload', 'error', 'Board not found');
        vscode.window.showWarningMessage(
          "Board not found. Make sure it's in bootloader mode — hold the button while plugging it in."
        );
        return;
      }
      this.reportTaskStatus('upload', err ? 'error' : 'success', err ? extractErrorSnippet(err) : undefined);
    });
  }

  private async simulate(): Promise<void> {
    const projectDir = this.getWorkspaceDir();
    if (!projectDir) {
      vscode.window.showErrorMessage('Open the folder containing your apio project (apio.ini) before simulating.');
      return;
    }
    if (!await this.offerCreateProjectIfMissing(projectDir)) {
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.reportTaskStatus('simulate', 'running');
    runInTerminal('FPGA Helper: Simulate', (out) => simulateProject(this.context, projectDir, out), (err) => {
      this.reportTaskStatus('simulate', err ? 'error' : 'success', err ? extractErrorSnippet(err) : undefined);
    });
  }

  private async test(): Promise<void> {
    const projectDir = this.getWorkspaceDir();
    if (!projectDir) {
      vscode.window.showErrorMessage('Open the folder containing your apio project (apio.ini) before running tests.');
      return;
    }
    if (!await this.offerCreateProjectIfMissing(projectDir)) {
      return;
    }
    if (!await this.confirmFirstRunIfNeeded()) {
      return;
    }

    this.reportTaskStatus('test', 'running');
    runInTerminal('FPGA Helper: Test', (out) => testProject(this.context, projectDir, out), (err) => {
      this.reportTaskStatus('test', err ? 'error' : 'success', err ? extractErrorSnippet(err) : undefined);
    });
  }

  private getHtml(): string {
    const htmlPath = path.join(this.context.extensionPath, 'src', 'panel', 'index.html');
    let html = fs.readFileSync(htmlPath, 'utf8');
    const iconUri = this._view?.webview.asWebviewUri(
      vscode.Uri.joinPath(this.context.extensionUri, 'resources', 'fpga.svg')
    );
    const solderedIconUri = this._view?.webview.asWebviewUri(
      vscode.Uri.joinPath(this.context.extensionUri, 'resources', 'soldered-logo.svg')
    );
    html = html.split('{{fpgaIconUri}}').join(iconUri?.toString() || '');
    html = html.split('{{solderedIconUri}}').join(solderedIconUri?.toString() || '');
    return html;
  }
}
