import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, spawnSync } from 'child_process';

export interface ProcessOutput {
  write(text: string): void;
}

function venvDir(context: vscode.ExtensionContext): string {
  return path.join(context.globalStorageUri.fsPath, 'apio-venv');
}

function apioBin(context: vscode.ExtensionContext): string {
  const dir = venvDir(context);
  return process.platform === 'win32'
    ? path.join(dir, 'Scripts', 'apio.exe')
    : path.join(dir, 'bin', 'apio');
}

function pipBin(context: vscode.ExtensionContext): string {
  const dir = venvDir(context);
  return process.platform === 'win32'
    ? path.join(dir, 'Scripts', 'pip.exe')
    : path.join(dir, 'bin', 'pip');
}

function findSystemPython(): string {
  const candidates = process.platform === 'win32' ? ['python', 'python3'] : ['python3', 'python'];
  for (const candidate of candidates) {
    const result = spawnSync(candidate, ['--version']);
    if (result.status === 0) {
      return candidate;
    }
  }
  throw new Error('No Python 3 installation found on PATH. Install Python 3 and try again.');
}

export class ProcessError extends Error {
  constructor(message: string, public readonly output: string) {
    super(message);
  }
}

export function runStreamed(command: string, args: string[], out: ProcessOutput, cwd?: string): Promise<void> {
  return new Promise((resolve, reject) => {
    out.write(`$ ${command} ${args.join(' ')}\r\n`);
    const child = spawn(command, args, { cwd });
    let combinedOutput = '';

    const collect = (chunk: Buffer) => {
      const text = chunk.toString();
      combinedOutput += text;
      out.write(text.replace(/\n/g, '\r\n'));
    };
    child.stdout.on('data', collect);
    child.stderr.on('data', collect);

    child.on('error', (err) => reject(err));
    child.on('close', (code) => {
      if (code === 0) {
        resolve();
      } else {
        reject(new ProcessError(`${command} exited with code ${code}`, combinedOutput));
      }
    });
  });
}

async function ensureApioInstalled(context: vscode.ExtensionContext, out: ProcessOutput): Promise<void> {
  const dir = venvDir(context);

  if (!fs.existsSync(apioBin(context))) {
    out.write('Setting up FPGA build environment (first run only)...\r\n');
    const python = findSystemPython();
    await runStreamed(python, ['-m', 'venv', dir], out);
    await runStreamed(pipBin(context), ['install', '--upgrade', 'pip', 'apio'], out);
  }

  await runStreamed(apioBin(context), ['packages', 'install'], out);
}

export function isApioInstalled(context: vscode.ExtensionContext): boolean {
  return fs.existsSync(apioBin(context));
}

export function bitstreamPath(projectDir: string): string {
  return path.join(projectDir, '_build', 'default', 'hardware.bin');
}

export function hasApioProject(projectDir: string): boolean {
  return fs.existsSync(path.join(projectDir, 'apio.ini'));
}

export function isEmptyDir(dir: string): boolean {
  return fs.existsSync(dir) && fs.readdirSync(dir).length === 0;
}

export async function buildProject(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  await ensureApioInstalled(context, out);
  await runStreamed(apioBin(context), ['build', '-p', projectDir], out);
}

export async function graphicalBuild(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  await ensureApioInstalled(context, out);
  out.write('Opening nextpnr — use its Pack/Place/Route controls to run through the design.\r\n');
  await runStreamed(apioBin(context), ['build', '-p', projectDir, '--gui'], out);
}

export async function simulateProject(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  await ensureApioInstalled(context, out);
  out.write('Opening GTKWave — close its window when done.\r\n');
  await runStreamed(apioBin(context), ['sim', '-p', projectDir], out);
}

export async function testProject(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  await ensureApioInstalled(context, out);
  await runStreamed(apioBin(context), ['test', '-p', projectDir], out);
}

export async function createProject(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  await ensureApioInstalled(context, out);
  await runStreamed(apioBin(context), ['examples', 'fetch', 'tinyfpga-bx/blinky', '-d', projectDir], out);
}
