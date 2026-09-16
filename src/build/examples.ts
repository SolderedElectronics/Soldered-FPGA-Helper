import * as fs from 'fs';
import * as path from 'path';

const EXCLUDED_ENTRIES = new Set(['_build', '.venv', '.DS_Store', '.git']);

export interface ExampleProject {
  id: string;
  dir: string;
}

export function listExamples(extensionPath: string): ExampleProject[] {
  const examplesRoot = path.join(extensionPath, 'examples');
  if (!fs.existsSync(examplesRoot)) {
    return [];
  }
  return fs.readdirSync(examplesRoot, { withFileTypes: true })
    .filter(entry => entry.isDirectory())
    .map(entry => ({ id: entry.name, dir: path.join(examplesRoot, entry.name) }))
    .filter(example => fs.existsSync(path.join(example.dir, 'apio.ini')))
    .sort((a, b) => a.id.localeCompare(b.id));
}

export function copyExampleProject(exampleDir: string, targetDir: string): void {
  fs.cpSync(exampleDir, targetDir, {
    recursive: true,
    filter: (src) => !EXCLUDED_ENTRIES.has(path.basename(src)),
  });
}
