# desktop-rescue

**Windowsの別Win32 desktopから、元の通常画面に戻るための独立した軽量常駐ツールです。** 常駐本体・CLIはネイティブC++の単一exeで、.NET、Python、Copilot CLI、WindowsComputerUseMCPを必要としません。必要な人だけ、別プロセスのSDKベースMCPアダプターを追加できます。

## 対象とできないこと

通常の入力desktop `Default` 上で起動すると、復帰先ハンドルを固定します。他のプログラムが作成した、**同じ対話ウィンドウステーション／ログオンセッション内のアクセス可能なWin32 desktop** に切り替わった場合、物理 **Home / Esc** でDefaultへの復帰を試みます。自ツール経由でのdesktop切替は前提にしません。

- 通常のDefaultではキーボードhookを置きません。異常側でだけ専用スレッドをdesktopに接続して監視します。入力内容・画面の記録はせず、注入されたキーを復旧トリガーとして扱いません。
- キーは奪わず元のhookチェーンへ渡します。異常側のアプリもHome/Escを受け取ります。切替境界のキーアップ等が元画面側に届かないことまでは保証しません。
- 復帰先は再学習・上書きせず、`SwitchDesktop` 後に実際の入力desktopがDefaultか再確認します。失敗・アクセス拒否・確認不能を成功扱いにしません。
- desktop切替イベントを利用し、**2秒間隔の入力desktop確認** をフォールバックにします。全desktop列挙・画面キャプチャは行いません。新desktopでhookが有効になるまで、イベント未達時は約2秒＋準備時間の空白があり、その間のキーは取り戻せません。画面が変わってすぐの一打で戻らない場合は少し待ってから押してください。
- 別ユーザー／別ログオンセッション、アクセス権のないdesktop、ロック画面、UACのsecure desktopは対象外です。認証操作・セキュリティ設定変更・権限昇格は行いません。
- Windowsの仮想デスクトップ（タスクビュー）、ウィンドウの前面化、ディスプレイ設定・ドライバーの不調とは別の機能です。黒い画面だけからWin32 desktopの問題と断定できません。
- Windowsのhookタイムアウトによる無通知のhook除去もあり、statusの `hookInstalled` は設置時の状態であって物理キー到達の保証ではありません。実機キー復旧と実desktop切替は未検証です。

詳細と公式API根拠: [設計と制限](docs/architecture.md)。

## ダウンロードとCLI

[GitHub Releases](https://github.com/monoqlo78/desktop-rescue/releases) の `desktop-rescue-win-x64.zip`、Windows on ARMでは `desktop-rescue-win-arm64.zip` を展開します。レビュー前の配布物はActionsのビルド成果物（公開Release前）としても提供します。ソースの公開・ZIPのダウンロードはPCへのインストールや自動起動登録を意味しません。

通常画面が表示されている間に、展開先から実行してください。管理者権限は不要です。

```powershell
.\desktop-rescue.exe start
.\desktop-rescue.exe status
.\desktop-rescue.exe recover
.\desktop-rescue.exe stop
```

`start` は同一ログオンセッションに一つだけバックグラウンド常駐を起動します。Default以外を入力desktopとしている時の新規起動は拒否します。`recover` はCLIからの明示的な復旧、`status` はPID・復帰先・入力desktop・hook・Win32エラー・資源情報のJSONを返します。エラー時の終了コードは1、使用法の誤りは2です。`stop` は停止要求を返し、常駐はhook・IPC・desktopハンドルを解放して終了します。終了確認は `status` の `not_running` を確認してください。

**自動起動は既定で無効です。** 必要な場合だけ利用者自身で以下を実行できます。今回は登録していません。

```powershell
.\desktop-rescue.exe autostart status
.\desktop-rescue.exe autostart enable   # 現在ユーザーのRunキーへ明示的に登録
.\desktop-rescue.exe autostart disable
```

展開した実行ファイルの移動前には停止し、自動起動を登録した場合は解除／登録し直してください。アンインストールは停止・任意の自動起動解除の後、展開したフォルダーを削除するだけです。

## 任意のCopilot CLI / MCP連携

常駐単体にMCPは不要です。必要な場合のみPython 3.10以降と [公式MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk) を使うアダプターを別プロセスで起動します。Python側のメモリは常駐本体の資源測定には含みません。

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\mcp\requirements.txt
```

Copilot CLIの `mcp-config.json` 等に登録する例です。絶対パスを実際の展開先に直してください。本ツールはMCPクライアント設定を自動変更しません。

```json
{
  "mcpServers": {
    "desktop-rescue": {
      "type": "local",
      "command": "C:\\Tools\\DesktopRescue\\.venv\\Scripts\\python.exe",
      "args": ["C:\\Tools\\DesktopRescue\\mcp\\server.py"],
      "env": {
        "DESKTOP_RESCUE_EXE": "C:\\Tools\\DesktopRescue\\desktop-rescue.exe"
      },
      "tools": ["*"]
    }
  }
}
```

MCPツールは `start / status / recover / stop` のみです。自動起動登録や任意コマンド実行は公開しません。MCPクライアントが停止しても、一度起動した常駐と物理キー監視は独立して動きます。

**WindowsComputerUseMCPとは別製品**です。[WindowsComputerUseMCP PR #1](https://github.com/monoqlo78/WindowsComputerUseMCP/pull/1) の短期復旧helperは同MCP経由の操作向けで、こちらの独立常駐の代わりではありません。併用は任意で、どちらも相手に依存しません。両方が動く場合の復旧競合を実機で検証したとは主張しません。既存リポジトリのコードは転載していません。

## ビルド・安全なテスト・配布

Visual Studio 2022以降のC++ Build ToolsとWindows SDK、PowerShellが必要です。常駐本体は外部パッケージ不要、静的CRTリンクの単一exeです。

```powershell
.\build.ps1 -Package
.\build.ps1 -Architecture arm64 -Package
python -m pip install -r .\mcp\requirements.txt
python -m unittest discover -s tests -v
.\artifacts\win-x64\desktop-rescue.exe self-test
.\artifacts\win-x64\desktop-rescue.exe probe --seconds 10
```

自己テストは偽のdesktopバックエンドによる復旧判定／検証失敗／競合と、一時的・ログオンSID限定IPCを検証します。hook設置・実desktop切替・自動起動登録はしません。`probe` は有界な観測専用プロセスで、通常の常駐endpointを使わず、キーボードhookも復旧も実行しません。ARM64クロスビルドはx64 Windows上でコンパイルのみとなり、ARM64実行テストはWindows on ARMが必要です。

CIはx64ビルド・自己テスト・MCPテストとZIP保存を実行します。手動 `Release Windows build` workflowでは、レビューした既存の `vX.Y.Z` タグからx64/ARM64 ZIPとSHA-256を生成しReleaseに添付します。ビルド手順は再実行可能ですが、異なるMSVC/SDKバージョンで同じバイト列になることは保証しません。

## リソース測定

常駐はメモリ／CPUが完全ゼロではありません。通常画面ではkeyboard hook 0、常駐プロセス1個、desktop切替通知＋2秒間隔の必要な入力desktop確認とIPC待機を行います。非Defaultの監視中はhook用スレッド1本が増えます。

```powershell
.\tools\Measure-Idle.ps1 -Executable "$env:LOCALAPPDATA\DesktopRescue\<version>\desktop-rescue.exe" -Seconds 180 -Output .\artifacts\idle.json
```

測定スクリプトは常駐起動やdesktop切替をしません。PIDと実行パスを照合し、安定化後の累積CPU差・経過時間・論理CPU数、メモリ／スレッド／ハンドル数を採取します。対象CPUと測定用PowerShellのCPUは別です。数分間のメモリ安定は長期リーク不存在の証明ではありません。実測条件・結果・未検証事項は [検証記録](docs/validation.md) に記載します。

## ライセンス・商用価格

非商用用途は [PolyForm Noncommercial 1.0.0](LICENSE) に従い無料。商用利用は **事前連絡・明示的許諾＋1シート150円の銀行振込、または1シート1米ドル相当Bitcoinの事前支払い** が必要です。問い合わせ先は **monoqlo78@gmail.com** です。[商用条件](COMMERCIAL-LICENSE.md) を確認してください。月額／年額は設定しておらず、寄付やOSI認定オープンソースとは案内していません。
