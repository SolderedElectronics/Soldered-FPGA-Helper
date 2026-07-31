import * as vscode from 'vscode';

import { FpgaHelperViewProvider } from './panel/FpgaHelperViewProvider';

export function activate(context: vscode.ExtensionContext) {
  const provider = new FpgaHelperViewProvider(context);

  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider('fpgaHelperWebview', provider, {
      // Without this, VS Code unloads the webview's page whenever it's
      // hidden (e.g. switching sidebar tabs) and reloads it from scratch
      // when shown again — wiping the port dropdown back to empty.
      webviewOptions: { retainContextWhenHidden: true }
    })
  );
}

export function deactivate() {}
