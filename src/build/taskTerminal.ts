import * as vscode from 'vscode';

/**
 * A minimal Pseudoterminal that just displays whatever gets written to it —
 * lets us run cross-platform Node logic (venv/pip/apio) while still showing
 * the output in a real Terminal panel, instead of typing shell-specific
 * script text via sendText (which breaks across bash/zsh/fish/PowerShell).
 */
export class TaskTerminal implements vscode.Pseudoterminal {
  private writeEmitter = new vscode.EventEmitter<string>();
  onDidWrite = this.writeEmitter.event;

  private closeEmitter = new vscode.EventEmitter<number>();
  onDidClose = this.closeEmitter.event;

  constructor(
    private readonly task: (out: { write(text: string): void }) => Promise<void>,
    private readonly onDone?: (err: Error | undefined) => void
  ) {}

  open(): void {
    this.task({ write: (text) => this.writeEmitter.fire(text) })
      .then(() => {
        this.writeEmitter.fire('\r\n\x1b[32mDone.\x1b[0m\r\n');
        this.closeEmitter.fire(0);
        this.onDone?.(undefined);
      })
      .catch((err) => {
        this.writeEmitter.fire(`\r\n\x1b[31m${err.message || err}\x1b[0m\r\n`);
        this.closeEmitter.fire(1);
        this.onDone?.(err);
      });
  }

  close(): void {}
}

// A Pseudoterminal-backed terminal exits (and can't be "restarted") once its
// task finishes, so re-running the same action would otherwise pile up a new
// tab in the terminal dropdown every click. Dispose the previous one for the
// same name first — one tab per action type, not one per run.
const activeTerminals = new Map<string, vscode.Terminal>();

export function runInTerminal(
  name: string,
  task: (out: { write(text: string): void }) => Promise<void>,
  onDone?: (err: Error | undefined) => void
): void {
  activeTerminals.get(name)?.dispose();

  const terminal = vscode.window.createTerminal({ name, pty: new TaskTerminal(task, onDone) });
  activeTerminals.set(name, terminal);
  terminal.show();
}
