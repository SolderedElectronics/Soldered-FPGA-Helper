import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { ProcessOutput, runStreamed, buildProject, bitstreamPath } from './apioEnv';

function loaderBin(context: vscode.ExtensionContext): string {
  const platformArch = `${process.platform}-${process.arch}`;
  const bin = process.platform === 'win32' ? 'openFPGALoader.exe' : 'openFPGALoader';
  return path.join(
    context.extensionPath, 'vendor', 'openFPGALoader-tinyprog', 'prebuilt', platformArch, bin
  );
}

export async function uploadProject(
  context: vscode.ExtensionContext,
  projectDir: string,
  out: ProcessOutput
): Promise<void> {
  const loader = loaderBin(context);
  if (!fs.existsSync(loader)) {
    throw new Error(
      `No prebuilt openFPGALoader for ${process.platform}-${process.arch} yet. `
      + 'See vendor/openFPGALoader-tinyprog/README.md to build it yourself.'
    );
  }

  // apio's build is incrementally cached (SCons-based) — always run it so an
  // edited source file never gets silently skipped in favor of a stale bin.
  await buildProject(context, projectDir, out);

  const bitstream = bitstreamPath(projectDir);
  out.write('\r\nMake sure the board is in bootloader mode before uploading.\r\n');
  await runStreamed(loader, ['-b', 'tinyFPGABX', bitstream], out);
}
