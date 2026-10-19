# 設計・安全境界

## 常駐とdesktop

常駐は非昇格の対話ユーザープロセスです。サービスやドライバーではありません。起動時の入力desktopが `Default` の場合だけ `DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP` でハンドルを保持します。名前だけで復帰先を開き直したり、異常画面で復帰先を再学習することはありません。非標準名の通常desktopはこの初期版では対象外です。

メインスレッドはWinEventとWin32メッセージを待ち、`EVENT_SYSTEM_DESKTOPSWITCH` と2秒タイマーで入力desktopを確認します。desktop作成そのものを通知するAPIへの依存や全desktop列挙はありません。別desktopが作成されても、まだ入力desktopでなければ監視スレッドは作りません。通知のdesktop間配送は保証せず、低頻度フォールバックを残しています。Default中の定期確認を完全に止めると、通知を受け取れない切替を検出できなくなるためです。

アクセス可能な非Default入力desktopが見つかると、新しい専用スレッドで以下を実行します。

1. `OpenDesktop(DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL)`。
2. ウィンドウ／hook作成前に `SetThreadDesktop`。
3. メッセージキューと `WH_KEYBOARD_LL` を作成して待機。
4. 物理Home/Escのkeydownだけを選び、カーネルイベントでメインスレッドに通知。callbackは直ちに `CallNextHookEx` に戻る。
5. メインスレッドで世代番号と現在の入力desktop名を再確認し、固定したDefaultへの復帰を試みる。

スレッド間の通知・停止にはカーネルイベントを使用します。desktop間の `PostThreadMessage` は権限制約があるため使いません。Defaultへ戻った／別desktopへ変わった／アクセス不能になった場合は旧hookを外し、スレッドを元desktopへ戻してハンドルを閉じます。全desktop分のプロセス・スレッドを常駐させません。

hook設置前の入力、Windowsによる無通知のhook除去、アクセス権限の差、入力確認とSwitchDesktopの間の外部切替、復帰直後に他プログラムが再切替する場合は完全に防げません。復帰を試す際は最長約500ms、入力desktopがDefaultになったかを確認し、それでも確認不能なら `verification_failed` を返します。これは確認時点の状態であり、その後も他プロセスが切り替えないという保証ではありません。

## IPC

名前付きパイプと重複起動防止mutexはログオンSID＋セッションIDの名前に分離し、保護DACLで当該ログオンSIDのみを許可します。パイプはremote clientを拒否し、最初のサーバーインスタンス以外の上書きを防ぎます。クライアントはサーバーへの識別レベル以上の偽装を許しません。

許可コマンドは固定された `status / recover / stop` のみ。コマンドサイズは64バイト、応答は最大64KiB、接続後の各読み書きには1.5秒の上限があります。受信後の応答確認も有界なので、読まないクライアントや接続したまま送らないクライアントで常駐を永久停止させません。同じログオンの悪意あるプロセスとの隔離境界ではありません（同じ資格情報のプロセスはCLI相当の操作を可能とします）。任意コード実行・シェル文字列・認証情報の交換はしません。

MCPは公式Python SDKにプロトコル処理を委ね、固定CLI引数を配列で渡します。shell実行はせず、12秒の上限、JSON形状・終了コードの確認を行い、実行失敗をツールエラーとして返します。MCP自体の追加メモリは任意の別プロセス負荷です。

## 参照した公式API仕様

- [LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc): hookスレッドのメッセージループ、短いcallback、注入キー、無通知のhook除去。
- [SetThreadDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddesktop): desktop所属と、既にwindow/hookを持つスレッドの制限。
- [PostThreadMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postthreadmessagew): desktop間配送の権限制約。これを避けてカーネルイベントを利用。
- [OpenInputDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openinputdesktop)、[SwitchDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-switchdesktop): 入力desktopとアクセス境界。
