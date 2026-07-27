import * as vscode from 'vscode';

import { FpgaHelperViewProvider } from './panel/FpgaHelperViewProvider';

export function activate(context: vscode.ExtensionContext) {
  const provider = new FpgaHelperViewProvider(context);

  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('fpgaHelperWebview', provider)
  );
}

export function deactivate() {}
