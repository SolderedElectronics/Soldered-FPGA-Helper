import * as fs from 'fs';
import * as path from 'path';

// Not bundled with the extension yet — these tutorial examples only exist on
// this dev machine, so point straight at the on-disk folder for now.
const EXAMPLES_ROOT = '/Users/fran/fpga/tutorial';

const EXCLUDED_ENTRIES = new Set(['_build', '.venv', '.DS_Store', '.git']);

export interface ExampleProject {
  id: string;
  dir: string;
}

export function listExamples(): ExampleProject[] {
  if (!fs.existsSync(EXAMPLES_ROOT)) {
    return [];
  }
  return fs.readdirSync(EXAMPLES_ROOT, { withFileTypes: true })
    .filter(entry => entry.isDirectory())
    .map(entry => ({ id: entry.name, dir: path.join(EXAMPLES_ROOT, entry.name) }))
    .filter(example => fs.existsSync(path.join(example.dir, 'apio.ini')))
    .sort((a, b) => a.id.localeCompare(b.id));
}

export function copyExampleProject(exampleDir: string, targetDir: string): void {
  fs.cpSync(exampleDir, targetDir, {
    recursive: true,
    filter: (src) => !EXCLUDED_ENTRIES.has(path.basename(src)),
  });
}
