.. _maint-vulkan-migration:

Vulkan migration plan
=====================

Status: implementation in progress, 2026-10-12. Baseline: ``239f8e66c``.
The Vulkan Viewer, Engine image processing, native OFX suites and shipped
plugin overlays are implemented. Legacy OpenGL renderer sources and build
dependencies have been retired and the Windows source retirement regression
has passed. CPU raster fallback passed its application matrix; native GPU
regressions passed against the updated host and IO bundle. Combined evidence
is recorded in ``build/vulkan-raster-fallback-proof.json``.
The subsequent OCIO CPU callback error boundary passed 88 fault injection cases,
84 transient retries, the 14 CPU configurations and 16 native GPU configurations.
That checkpoint is recorded in ``build/vulkan-ocio-callback-fallback-proof.json``.
Managed memory limits, actual allocation refusals, OFX CPU retries and runtime
Viewer raster fallback now have a separate 14-case hardware matrix in
``build/vulkan-memory-atomic-tests/report.json``. Its integration with normal
CPU/native regressions is checked by ``tools/verify-raster-fallback.py``;
the combined output target is ``build/vulkan-memory-fallback-proof.json``.
Cached native input under actual heap pressure also passed on RTX, AMD and
SwiftShader. Its separate evidence is recorded in
``build/vulkan-pressure-settled-fixture-hardware-tests/report.json`` and
``build/swiftshader-pressure-settled-fixture-tests/report.json``.
Concurrent Viewer pressure passed 12 configurations on those three devices.
Current pressure evidence, copied pixels and source/import audits are combined
in ``build/vulkan-memory-pressure-proof.json``.
Logical-device retirement and recreation are implemented in the Engine, native
OFX suites, OCIO/Shadertoy and both sharing Viewers. Deterministic loss-result
tests are recorded in ``build/vulkan-device-loss-proof.json``. These tests keep
the physical desktop driver running; full migration remains incomplete.
Full-size 4K/8K application OCIO processing is now measured through NatronRenderer
with CPU/native graphs and validation enabled/disabled on RTX/AMD. Its bounded
evidence is recorded in ``build/vulkan-application-performance-proof.json``;
Real 4K/8K cold-cache timeline seeks and independent Viewer presentation are
measured in ``build/vulkan-viewer-performance-proof.json``. Windows continuous
playback passed 16 configurations and 112 scheduler stages. The RTX legacy GL
comparison and sampled adapter memory are now recorded in
``build/windows-renderer-api-comparison-final-optimized-proof.json``. Physical
scanout has not been measured. Parsec testing is
excluded from the completion criteria by user request.
Earlier sections retain implementation checkpoints. The final source retirement
section describes the current configuration. The completion target is Windows;
Linux/macOS validation is outside this task.

完了判定の範囲
--------------

2026-10-12のユーザー指定により、**Windows版の完了をこの移行タスクの完了とする**。
Linux X11/WaylandとmacOS MoltenVKの実機ビルド・検証は今回の完了条件から外す。
Parsec試験もユーザー指定により完了条件から外す。既存の検証結果に残る
``parsecTested=false`` 等の記録は、試験を実施していない事実を示す。
以前のcheckpointにあるParsecの未検証記述は、現在の残作業には含めない。
以降の他OSに関する記述は将来の検証計画またはソース変更の履歴である。
Windowsの機能・安定性・依存除去の条件は引き続き満たす必要がある。
この範囲に限定した同一buildの統合照合結果は
``build/vulkan-windows-application-integration-proof.json`` に保存する。
従来のarchiveと検証結果は、当時の完了範囲のまま保持する。

OpenGLのソース削除とVulkan完全移行の完了を区別する。Windowsでの
描画経路・配布OFX・依存監査の統合証拠は
``build/vulkan-native-ofx-retirement-proof.json`` に保存するが、この証拠だけでは
全移行段階の完了を示さない。回帰テスト中のソースと実行ファイルは固定する。
OpenGL除去時点のソース・実行ファイルと検証結果は
``build/native-raster-integration-baseline/manifest.json`` に保存し、CPU退避経路の
検証結果とは区別する。

2026-10-12の完了条件と現在の検証範囲は次のとおり。

.. list-table:: Windows受入条件と検証状況
   :header-rows: 1
   :widths: 18 45 37

   * - 段階
     - 条件
     - 現状と次の検証
   * - 0 / 性能
     - 4K/8K、p50/p95、転送量・VRAMピークの基準比較。
     - RTX/AMDの描画コア24条件に加え、NatronRendererの4K/8K全画像OCIO処理を
       CPU/native・Validation有効/無効の16条件で計測した。実Viewerの4K/8K
       フレーム移動も64条件・1280標本で確認した。複数Viewerの連続再生・停止・
       逆再生は16条件・112操作を通過した。旧GLとの6プロセス・240標本の比較と
       adapter全体のメモリを記録した。性能改善も実装・再計測済み。
       他プロセスを含む定期標本であり、瞬間・process単位のVRAMピークは証明していない。
   * - 3
     - VRAM上限、確保失敗からのCPU再試行、GPU専用処理の明確な失敗、
       device lostからの再初期化。
     - 管理対象メモリ上限、実確保拒否、CPU再試行とViewerのCPU表示への切替を
       両GPUで検証した。キャッシュされたGPU入力と同時Viewerの逼迫試験も
       RTX/AMD/SwiftShaderで通過した。device lostの結果を模擬注入した論理deviceの
       再作成も検証した。物理driverのTDRや取り外しは実行していない。
   * - 4
     - 全配布IDの対応表と、非対応外部OFXの条件・UI説明。
     - 全579バージョンのrender / interact宣言を対応表へ記録し、外部OFXの
       UI説明を検証した。全ノードの全機能一致は引き続き別の証拠を必要とする。
   * - 5 / CI
     - WindowsのソフトウェアVulkan、配布パッケージの再現性。
     - 固定SwiftShaderで描画コア9条件と配布Python/Renderer/Viewerをローカル検証。
       同一入力のZIP再生成一致、Unicodeパスへの移設、実行moduleの由来も確認。
       Windows CI定義は作成した。remote CIとSDK未導入のclean machineでは未実行。

Windows性能改善と配布候補
-------------------------

ユーザーの指定により、Windows完了前の性能改善を今回の作業に含めた。
CPU/GPU境界では全画像の一時vectorと余分なmemcpyを削減し、必要なROIを
RAMの行strideに直接読み書きする。マスクなし・Mix=1・入出力の全boundsが一致する
OCIO処理は、同じshaderで入力から出力へ直接描画し、2つの全画像作業領域と
2回のGPUコピーを省く。部分領域、ずれたbounds、マスク、Mixの経路は元の意味を保つ。
CPUのNaN検査とbitmapの行走査はSSE2で複数要素をまとめて検査し、他のplatformは
scalar経路を使う。Infinity、signed zero、subnormal、NaN payloadとROI外の値を
含むbit単位の回帰試験、およびbitmap/trimapのblock境界試験を追加した。

改善前のソースと実行ファイルは ``build/performance-before-optimization`` に保存し、
hashを元の計測入力と照合した。改善後の同一条件の20標本の中央値は次のとおり。

.. list-table:: NatronRenderer GPU経路の改善前後
   :header-rows: 1
   :widths: 28 12 20 20 20

   * - device
     - 解像度
     - 改善前 ms
     - 改善後 ms
     - 短縮率
   * - RTX 5080
     - 4K
     - 242.66
     - 215.71
     - 11.1%
   * - RTX 5080
     - 8K
     - 730.56
     - 599.78
     - 17.9%
   * - AMD Radeon Graphics
     - 4K
     - 287.10
     - 271.46
     - 5.4%
   * - AMD Radeon Graphics
     - 8K
     - 1018.77
     - 826.89
     - 18.8%

全画像CPU source、配布OCIO Linear→sRGB、分散した64×48のCPU PNG probeの
wall timeであり、API単体やViewerのfpsの測定ではない。別時刻の計測なので、
CPU/system負荷の揺れを含む。CPU経路はRTX 4Kで7.5%改善した一方、RTX 8Kで1.2%、
AMDで約4.8〜4.9%遅くなった。全480標本・1,474,560画素を独立照合し、現行CPU/GPU
出力の最大誤差は1/255以内だった。詳細は ``build/vulkan-optimization-proof.json``。

同じ最終比較runの旧GL中央値はRTX 4Kが156.81ms、8Kが611.48ms。
Vulkanの4Kは37.6%遅い。8Kは同程度だが旧GL出力は13/255の誤差で精度基準を満たさず、
旧IOのOCIO 2.1と現行の2.6の違いもあるためAPIだけの比較とはしない。
driverメモリは20ms間隔を要求したadapter全体の最大観測値で、実間隔も記録する。
他processとdriver内部のWSI moduleを含む。瞬間の専有ピークとは区別する。

改善後は両GPUで転送/ROI/padding、Engine画像変換、NaN、device lost、
OCIOマスク/Mix/HDR/alpha/proxy/cacheを再検証した。SwiftShaderでも描画コアを確認した。
同じ更新buildのメモリ不足14条件は ``build/final-optimized-memory-proof.json``、
RTX/AMD/SwiftShaderの論理device再作成15条件と描画コア3条件は
``build/final-optimized-device-loss-proof.json`` に保存した。独立照合した画素数は
それぞれ9,291,776と6,045,696。物理driverはresetしていない。
ローカルRelease配布候補は ``build/Natron-windows-qt6-vulkan-release.zip`` に保存し、
起動手順は ``INSTALL_WINDOWS.md`` に記載する。配布候補の照合結果は
``build/windows-portable-final-proof.json``。これは署名済みinstallerやremote CI、
clean machineの試験、物理driver TDRの証拠ではない。全ノードの全機能一致も
宣言の全件検査だけでは判断しないため、全移行完了のflagはfalseを保持する。

NVIDIAのVulkan WSIが内部で読み込むドライバDLLは、NatronがOpenGLを呼び出す
経路とは区別して記録する。所有するソース・リンク・OFXのOpenGL依存は許容しない。

``tools/audit-native-ofx-compatibility.py`` で全配布OFXの登録を移行前のCSVと
照合した。579バージョン・556 IDに追加・欠落はなく、各行の宣言を
``build/ocio-failure-ofx-compatibility/plugins.csv`` に保存した。Overlayのentry point等の
pointerプロパティはキャッシュに保存されないため、表では ``not-cached`` として
扱う。登録・宣言の一致と、実レンダー・編集操作の証拠は別々に判定する。

DrawSuite宣言の全件検査と外部OFXの説明
--------------------------------------

pointerプロパティを実バイナリのDescribe / DescribeInContextから取得する検査では、
579バージョン・556 ID・1,193 contextを照合した。旧Custom Interactのhost capabilityを
有効にした追加Describeでも同じ宣言を得た。interact自身のDescribe / Create / Drawは
呼ばず、OpenGL contextも作らない。

この検査でMiscのCheckerBoard、ColorBars、ColorWheel、Constant、Rand、Radial、
Rectangle、Solidの8ノードに、Generator共通コードのV1登録が残っていた。
描画クラスは既にDrawSuite adapterへ移行していたが、``generatorDescribe`` が
旧SDKの登録関数を直接呼んでいた。Misc / Arena / IOのSupportExtで
``NatronOFX::setOverlay`` に統一し、現在の579バージョンに未置換のinteract宣言はない。
変更前後の宣言は ``build/drawsuite-inventory/native.json`` と
``build/drawsuite-inventory/after-native.json`` に別々に保存する。
登録と機能検証を混同しない。

Natron本体はViewer V1 entry pointを選択せず、Describe / Createを含めて旧interactを
呼ばない。V2のないdescriptorを直接取得しようとした場合も明確な例外にする。
旧カスタムparameter UIとparametric背景もnative entryのみ選択する。
V1とnativeの両方がある場合はnativeを使う。

未置換のViewer overlay、custom control、曲線背景を持つ外部OFXでは、main parameter
pageに警告を表示し、tooltipとInfo pageで対象のcontrolを説明する。
確認ダイアログは追加しない。警告は保存・評価対象外のhost knobとする。
CPUノード自体は除外しない。interact宣言のないノードにDrawSuiteは要求しない。
外部OFXを一括除外するpolicyは導入していない。

独立したOFX ABI fixtureでV1-only Viewer overlay、V1-only custom control、interactの
ないCPUノードを区別する。実hostで作成・編集・描画・破棄を行い、全legacy callback
が0回、ordinary parameterとCPU出力が維持されることを確認する。
Generatorは8ノードすべての線・handle・pen編集・focus復帰・Undo / Redoを検証する。
Vulkanと明示選択したCPU raster表示を100% / 200%、8u / 32f、RTX / AMDで実行する。
GPU失敗時のfallback理由と、テスト用の明示CPU選択は別の状態として判定する。
32 GUI process、128 Generator case、48 legacy / no-UI caseが通過した。
CPU fixtureの147,456画素はRGBA完全一致、legacy callbackは全段階で0回である。
連続した同じknobの編集は1つのUndo commandへmergeされるため、Undoは2回のdragの
前の値へ戻る。初回の照合scriptが2回目のdrag直前を期待していた誤りを修正した。
GUI run時のscriptは ``build/verify-ofx-interact-migration-recorded.py`` に保存し、
修正版はその記録hashと実際の照合scriptのhashを別々に残す。
その他のGUI test、source、runtime入力は現在のhashと完全一致を要求する。

.. code-block:: powershell

   tools/audit-windows-ofx-interacts.ps1 -ReportDirectory build/windows-ofx-interact-inventory
   tools/test-windows-vulkan-ofx-interacts.ps1 -ReportDirectory build/windows-vulkan-ofx-interact-final-tests
   uv run --frozen python tools/verify-ofx-interact-migration.py --matrix build/windows-vulkan-ofx-interact-final-tests/report.json

保存済みの上記runを再照合する場合だけ、最後のcommandへ
``--recorded-verifier build/verify-ofx-interact-migration-recorded.py`` を指定する。
新しいGUI runでは現在の照合scriptが記録されるので、このoptionは不要である。

この証拠の範囲は全interact宣言と上記UI / CPU fixtureであり、全配布ノードの全render
functionや、Windows配布package・性能条件の完了を示すものではない。

OpenGL必須の外部OFXとrender宣言の対応表
-----------------------------------------

``OfxImageEffectPropOpenGLRenderSupported`` は互換性判定用のmetadataとして保持する。
SDKのrender suite、context、texture経路は復活させない。
``false`` と未宣言はCPU、``true`` はCPU実装を持つ任意OpenGL処理として扱う。
``needed`` かつnative Vulkan未対応のcontextは、effectのCreate callbackより前に
拒否する。既存のnode作成エラー画面とstderrで、CPUまたはVulkan対応版の導入を案内する。
任意OpenGLの宣言から、動作しない旧GPU設定controlを再表示しない。
BeginSequence / Render / EndSequenceのOpenGL enabled値は常に0にする。

``needed`` とVulkan対応を併せて宣言するノードはVulkan必須として実行する。
GPUを使用できない場合もCPU callbackへ退避させず、明確に失敗する。
独立したOFX ABI fixtureは、rootでの必須宣言、contextでの必須宣言、任意OpenGLの
CPU実装、明示CPU、未宣言CPU、旧必須宣言とnative Vulkan実装の6種類を持つ。
最後の種類はopaque clip imageを実Vulkan copyで出力する。
fixture自体にはOpenGL / Vulkan / Qtをlinkしない。

古いcacheから必須宣言が消えていた場合に備え、OFX cache形式を
``NatronOFXCachev2`` へ更新する。テストでは専用profileのcacheだけを引き継ぎ、
旧v1形式でrender metadataを除いたcacheからも再走査する。
利用者の設定とcacheをテスト用に書き換えない。

全同梱OFXのDescribeは、実hostと同じnative suiteのavailabilityを提示する。
診断用suiteのruntime関数は呼び出しを記録して拒否し、GPUを初期化しない。
``build/retired-render-native-shipped-inventory/plugins.csv`` の579バージョン・556 IDは、
CPUが562、CPU / Vulkanが16、Vulkan必須が1で、未置換のrender / interact宣言は0件である。
Describe中のruntime Vulkan呼び出しも0回である。instanceのparameter変更による
capability切替や、各CPUアルゴリズムの機能一致はこの宣言表だけでは検証されない。

.. code-block:: powershell

   tools/audit-windows-ofx-interacts.ps1 -ReportDirectory build/retired-render-native-shipped-inventory
   tools/test-windows-ofx-retired-render.ps1 -ReportDirectory build/windows-ofx-retired-render-final-tests
   uv run --frozen python tools/verify-ofx-retired-render.py --matrix build/windows-ofx-retired-render-final-tests/report.json --output build/windows-ofx-retired-render-final-proof.json
   tools/test-windows-ofx-retired-render-headless.ps1
   uv run --frozen python tools/verify-ofx-retired-render-headless.py

NatronRendererのRTX / AMD / Vulkan無効化の3 processでも、18 fixture caseと
33,792画素が通過した。このうち6,144画素はnative clip経路である。
テストランナーは専用interpreterへ検査入力を渡してstdinを閉じ、EOF markerと
正常終了を確認する。出力完成後も入力待ちで残った初期pilotは成功証拠に含めない。
GUIの最終22 processでは132 fixture case、258,048画素が通過し、そのうち55,296画素は
native clip経路である。OpenGL必須の2種類は44回すべてCreate callbackより前に拒否した。
v1 / v2 cacheからの起動でも同じ結果であり、利用者の設定hashは前後で一致した。
この結果は ``build/windows-ofx-retired-render-final-proof.json`` と
``build/windows-ofx-retired-render-headless-proof.json`` に保存する。
再実行する場合は新しいreport directoryを指定する。

前節の32 GUI processは、その時点のbuildと保存済みcheckpointの証拠である。
render互換性判定の追加後のsource / binaryと混同しない。

CPUラスタ候補の証拠は ``build/native-raster/candidate-proof.json`` に保存した。
GPU loaderを初期化しない33の画素・タイル・不正入力試験、関連8ソースの
コンパイル、RTX/AMDそれぞれ5,184描画条件・1,886,976チャンネル値のVulkan比較が
通過した。LUT、A/B・wipe、premult、補間、PAR、非ゼロ原点、top-down、
部分描画の合成を含み、最大差は1e-6未満、Validationエラーは0である。
これは独立候補の証拠である。統合後の起動・実Viewer操作・ヘッドレス描画は
``tools/test-windows-raster-fallback.ps1`` で別途検証した。
GPU無効化・ドライバ不在の計14構成が通過し、14 OFXレンダーの43,008画素、
16ハンドルの編集・Undo/Redo・フォーカス、68キャプチャを確認した。
結果は ``build/raster-fallback-environment-tests/report.json`` に保存した。
各試験で変更した環境変数の復元と、利用者の設定ファイルが変更されていないことも
確認した。更新後のホスト・IOでネイティブGPU側の16構成とCTestの2試験を再実行し、
RTX/AMD各5,184描画条件の比較、730ソースと332 PEファイルの依存監査を統合した。
PNGのCPU/native比較は全チャンネルを確認し、RGBは8bit値で最大1の差、
alphaは完全一致を条件とする。比較元のPNGも各テスト結果に保存する。
``NATRON_VULKAN_DISABLED=1`` はGPU loaderを呼ばず、CPU画像処理とQWidgetの
ラスタ表示を使う。Vulkanの初期化に失敗した場合も、同じプロセス内では
GPUへの再変換を試みずCPU経路を使う。GUI試験は8bit/float、画面倍率1/2で
パン・ホイール拡大と表示画素を確認し、専用設定・起動マーカー・PID制限時間で
起動時／終了時の確認ダイアログを検出する。
OFXからの初期化失敗も共有ランタイムに記憶し、待機中のスレッドは初期化を
再試行しない。CPU画像を使うOCIOの任意GPU処理は、context生成に失敗したら
既存のCPU processorに戻る。これによりプラグイン側の例外がCPU処理の
コールバックから外へ出ることを防ぐ。GPU専用ShaderToyは成功画像を生成せず
失敗することも検証した。

OCIOのCPU画素への任意GPU処理では、初期化後のsuite失敗もコールバックの中で
受け止める。device情報は画素の変換前に取得し、変換結果はupload・実行・
readback・一時画像解放がすべて成功した後に確定する。失敗時は元の画素と
行末の余白を保持し、既存のCPU processorで一度だけ変換する。
``OcioVulkanFailureTest`` は7段階で3種類のsuiteエラーを注入し、RGB/RGBA・
正負のstrideで88条件と84回の再試行を検証する。これはコールバックの
失敗境界とCPU退避の試験であり、実ドライバのOOM・device lostの復旧とは区別する。

管理対象メモリ上限と確保失敗からの復旧
------------------------------------------------------------------------

``NATRON_VULKAN_MEMORY_LIMIT_MB`` はVMAが確保するdevice-local heapの
``VkDeviceMemory`` block総量にMiB単位の上限を設定する。未指定または0では
明示上限を設けない。環境変数は論理deviceの生成時に読み取る。
``VK_EXT_memory_budget`` が利用できる場合は有効化し、
VMAの予算確認も使用する。画像をdevice-local memoryへ確保し、別heapへの
退避による上限の回避を防ぐ。swapchainやドライバ内部の確保はVMA管理の外にある。
UMAではdevice-local heapに属するhost-visible stagingもこの上限に含まれる。

メモリ統計は確保callbackでblock総量とピークを数え、VMAのheap別統計と照合する。
32 MiB上限の実機試験ではRTX 5080のピークが33,554,432 byte、AMDが
31,853,824 byteだった。各GPUで3回の実確保拒否と解放後の3回の再利用、
48回の並行転送を確認し、Validationエラーは0だった。

Vulkanのメモリ不足は結果コードを保持する例外として扱い、OFX C ABIの境界で
``kOfxStatErrMemory`` に変換する。Engineは一時画像とロックを解放してから、
失敗したnodeのcacheを消去し、RAM出力でCPU再試行する。GPU専用nodeは
``NatronOfxImageEffectPropVulkanRenderRequired`` を宣言し、CPU実装を呼ばず
明確なエラーを返す。配布Shadertoyにもこの宣言を追加した。

独立C ABI fixtureは実際のhost suiteで256 MiB画像の確保を要求する。
32 MiB上限ではOFX render actionがメモリ不足を返し、CPU参照画像と3,072画素が
一致する。別processの2048×2051画像ではOFX actionより前の画像確保と、
RAM出力のnative変換が上限を超える条件を検証し、4,200,448画素が一致した。
GPU専用fixtureはどちらの条件でもCPU renderを行わず出力ファイルを生成しない。
同じsuiteを使う正常系generatorも、通常予算のEngine回帰試験で実レンダーする。

ViewerはRAMアップロード、native image転送とframe描画の確保失敗からCPU表示へ
切り替える。native windowのevent処理中は切替をqueueし、自身をその場で削除しない。
display textureとrendererを解放して再描画し、GPU専用入力はCPU実装を呼ばず停止する。
診断には失敗した経路とVulkan結果コードを表示する。
64 MiB上限の試験は正常な表示を確認してから画像転送や画面サイズを変更する。
両GPUでRAMアップロード、native転送、8u/32fのframe描画の8条件がCPU表示へ復旧し、
Validationエラーと確認ダイアログは0だった。native転送の試験はgraph切替中の
転送拒否と表示の復旧を確認するもので、切替先OFXのrender完了を条件にはしない。

設定、cache、開始markerとstdout/stderrはテスト専用profileへ保存する。
GUI fixtureはQt event loopを直接終了し、保存確認を開かない。watchdogは
起動・実行・終了のどこで停止したかを記録し、所有するtest PIDだけを終了する。
元の利用者設定のSHA-256は全試験の前後で一致した。
この試験は管理対象確保の拒否であり、物理GPUを失うfaultからの再初期化は含まない。

.. code-block:: powershell

   tools/test-windows-vulkan-memory.ps1 -ReportDirectory build/vulkan-memory-tests
   tools/test-windows-vulkan.ps1 -Tests MemoryBudget

キャッシュ済みGPU入力を保持した確保失敗
------------------------------------------------------------------------

``Tests/VulkanPressurePlugin.cpp`` はEngine/QtへリンクしないOFX C ABI fixtureで、
641×513のRGBA32F入力をnative cacheへ保持する。64×48のnative Inspect nodeを
経由してWriterへ渡すため、事前レンダーで入力全体のCPU snapshotを作らない。
別のInspect nodeで入力がnative imageとして渡されること、sourceの再レンダーと
CPUからのuploadが0であることを確認してから、共有deviceへ512×512の画像を
保持し続け、32 MiB上限で実際の ``kOfxStatErrMemory`` を発生させる。

その状態で全体画像を必要とするfilterをレンダーすると、出力のnative確保が
失敗してCPUへ再試行する。RTX/AMDではキャッシュ入力を読み戻し、SwiftShaderでは
読み戻し用の確保も不足したため、sourceをCPUで1回再レンダーして成功した。
圧迫用画像を解放した後には、新しいnative sourceとInspectがCPU再試行なしで
成功する。各構成で4出力の12,288画素をCPU参照と完全一致で比較した。

``tools/test-windows-vulkan-pressure.ps1`` が専用profile、開始marker、interpreterの
EOFとPID watchdogを使用してこの順序を再現する。RTX/AMDの結果は
``build/vulkan-pressure-settled-fixture-hardware-tests/report.json``、SwiftShaderは
``build/swiftshader-pressure-settled-fixture-tests/report.json`` に保存した。
3構成すべてでValidationエラーとOpenGL DLLの読み込みは0、起動・終了は制限時間内、
利用者設定のSHA-256と実行ファイル・fixture・関連sourceのhashは試験前後で一致した。
この追加試験のためのEngine修正は不要だった。既存の統合checkpointはその実行時点の
証拠として保持し、この結果は別reportで扱う。同時Viewerは次の試験で検証し、
device lostは別の受入条件として保持する。

.. code-block:: powershell

   tools/test-windows-vulkan-pressure.ps1 -ReportDirectory build/vulkan-pressure-tests
   tools/test-windows-vulkan-pressure.ps1 -GpuIndices 0 -IcdPath '<software-ICD.json>' -ReportDirectory build/software-pressure-tests

同時Viewerの共有メモリ逼迫とCPU再試行
------------------------------------------------------------------------

``tools/test-windows-vulkan-concurrent-memory.ps1`` は2つのViewerを同時に表示し、
Engineと同じ論理deviceを使うnative入力を実際に描画してから、64 MiB上限で
共有heapを画像確保によって埋める。両Viewerの描画領域を480×360から1200×900の
物理pixelへ変更すると、両方のframe確保が ``VkResult -2`` を返してCPU表示へ
切り替わる。初期化前に失敗する試験ではない。

切替によって解放されたメモリも再び画像確保で埋め、自動再描画が落ち着いてから
独立した2つのfilterを要求する。OFX fixtureの ``requestToken`` で各要求のCPU actionを
区別し、10秒の上限を持つbarrierで、2つが同時に実行中であることを確認する。
これは試験内の同期であり、Vulkanの結果コードは注入しない。確保拒否は実際の
host suiteによるもので、2つのfilterはキャッシュ済み入力を使ってCPUへ再試行する。

圧迫用画像を保持したまま、両Viewerの出力が初期の緑からfilter処理後のcyanへ
更新されたことを検証する。画面中央60%の全RGBA値を参照色と完全一致で比較し、
メモリ解放後の表示も確認する。新しい3つ目のViewerでは、初期と同じ論理deviceを
使ってnative表示を再開する。各構成で7枚、1,741,824画素を比較した。

8u/32fと画面倍率1/2のRTX/AMD 8構成、SwiftShader 4構成が通過した。
結果は ``build/vulkan-concurrent-memory-settled-hardware-tests/report.json`` と
``build/swiftshader-concurrent-memory-settled-tests/report.json`` に保存した。
画面倍率を変更する専用profileでは、所有するテストwindowを主画面の表示範囲内へ
配置し、native surfaceの露出状態も記録する。起動・終了のwatchdogと保存確認を
開かない終了処理は従来と同じで、利用者のwindow配置と設定には触れない。

``tools/verify-vulkan-memory-pressure.py`` はこれら12構成とキャッシュ入力の3構成を
再照合し、合計20,938,752画素、733 source、332 runtime PE fileとOFX fixtureの
通常／delay importを確認した。Validationエラー、QtのOpenGL moduleと確認dialogは0、
利用者設定のSHA-256は一致した。NVIDIAのWSIによるsystem OpenGL DLLの読み込みは
別に記録し、所有するsourceとlink/importにGL依存がないことを監査する。
この試験でもEngine/Viewer実装の追加修正は不要だった。
統合証拠は ``build/vulkan-memory-pressure-proof.json`` に保存する。

.. code-block:: powershell

   tools/test-windows-vulkan-concurrent-memory.ps1 -ReportDirectory build/concurrent-memory-tests
   tools/test-windows-vulkan-concurrent-memory.ps1 -GpuIndices 0 -IcdPath '<software-ICD.json>' -ReportDirectory build/software-concurrent-memory-tests

device lostの失効と論理device再作成
------------------------------------------------------------------------

``VK_ERROR_DEVICE_LOST`` を受けた論理deviceは失効状態を共有し、古い画像、
program、commandの利用を拒否する。記録中に別threadが失効を検知した場合も、
queueへの送信直前と完了後に検査する。失効したmapped memoryの内容は返さない。
旧deviceの資源は、そのdeviceで明示的に破棄するため所有を維持する。
新しいdeviceへ旧handleを付け替えない。

shared context poolは失効したcontextを再利用せず、同じphysical deviceとinstanceに
新しい論理deviceを1つ作る。Engineは旧contextのcache entryを確保・読戻し前に
除外し、入力を再取得して1回だけ再描画する。復旧中の再度の喪失は明確な失敗に
する。GPU必須Shadertoyも新しいprogramを作って再描画する。OCIOは保持していた
program、補助shader、LUTとprocessor cacheを作り直す。

native OFX suiteは ``kNatronOfxStatErrDeviceLost`` (1001) を返す。
context、image、programの解放は失効後も可能だが、旧handleは復活しない。
Viewerはwindowのrender callbackを抜けたGUI turnで旧surfaceと表示画像を解放し、
新しい共有deviceで描画窓を作り直す。遅れて到着した旧画像は破棄して再要求する。
新しい窓が一度も描画できずに再度失効した場合は、自動再作成を繰り返さない。

``NatronOfxVulkanTestSuite`` は専用 ``NATRON_TEST_PROFILE`` 内でだけ公開する。
queue submit前、実処理の完了後、present前に結果コードを模擬注入する。
物理driverのTDRやGPU取り外しを起こす試験ではない。C++ testは各経路の失効、
126回の旧device操作拒否、旧資源を保持した新deviceの描画、記録中の並行失効と
全資源の破棄をValidation付きで確認する。

Natron側では、キャッシュされたnative入力の再描画、OFX render action内の喪失、
保持済OCIO processorとGPU必須Shadertoyの復旧を確認する。2 Viewerの共有device
喪失は8u/32f、画面倍率1/2の各組み合わせで確認する。専用profile、開始marker、
interpreterのEOF、GUI event loopの終了とwatchdogを使い、保存確認を開かない。
各構成の結果、画像、利用者設定のhashとsource/binary hashは別directoryへ保存する。

``tools/verify-vulkan-device-loss.py`` はhardware/software report、実ファイルのhash、
コピーしたPNGの全比較対象RGBA値、起動・終了ログ、sourceとPE importを再照合する。
RTX/AMDの10構成とSwiftShaderの5構成で計6,045,696画素が一致し、Validationと
想定外の確認dialogは0、利用者設定のSHA-256は一致した。記録中の並行失効を
含むC++ testも3 deviceで通過した。監査対象は736 sourceと332 runtime PE fileである。
完全移行の未完了条件を保持した統合証拠は ``build/vulkan-device-loss-proof.json`` に
保存する。直前のメモリ逼迫checkpointは
``build/memory-pressure-integration-baseline/manifest.json`` と ``snapshot.zip`` に保持する。
ソース変更後は旧hashを更新せず、現ビルドに対して逼迫試験を再実行する。

復旧処理を含む同じビルドで、実メモリ逼迫の15構成・20,938,752画素と通常の
native Engine描画を再検証した。新しい結果は ``build/device-loss-pressure-hardware-tests``、
``build/device-loss-pressure-software-tests``、``build/device-loss-concurrent-memory-hardware-tests``、
``build/device-loss-concurrent-memory-software-tests``、``build/device-loss-native-engine-tests`` の
各 ``report.json`` に保持する。``verify-vulkan-memory-pressure.py`` に
``--device-loss-proof build/vulkan-device-loss-proof.json`` を渡し、同一source/runtimeの
復旧証拠も照合した現在の統合結果が ``build/vulkan-memory-pressure-proof.json`` である。

.. code-block:: powershell

   tools/test-windows-vulkan.ps1 -Tests DeviceLoss
   tools/test-windows-vulkan-device-loss.ps1 -ReportDirectory build/device-loss-tests
   tools/test-windows-vulkan-device-loss.ps1 -GpuIndices 0 -IcdPath '<software-ICD.json>' -ReportDirectory build/software-device-loss-tests

ソフトウェアVulkanの描画コア検証
------------------------------------------------------------------------

Windowsにインストール済みのChrome 155.0.8059.39に含まれるSwiftShaderを、
process限定のICD選択で使用した。probeは物理deviceが1つだけで、typeがCPU、
runtime APIが1.3であることを確認する。既存のGPU選択やdriver登録は変更しない。
`SwiftShaderの公式説明 <https://github.com/google/swiftshader/blob/master/README.md>`_
でもCPUによるVulkan 1.3実装として提供されている。

``tools/test-windows-vulkan-software.ps1`` はCompute、MemoryBudget、Renderer、
ViewerDisplay、ViewerGrid、ImageOperations、FragmentProgram、FragmentLutの
8つのheadless試験を実行し、Validationエラーがないことを確認する。
結果は ``build/swiftshader-frozen-core-tests/report.json`` に保存した。
ICD manifest、DLL、各実行ファイルと描画コアのsource hashは試験前後で一致し、
環境変数の復元も確認した。``VK_EXT_memory_budget`` がないdriverでも、
32 MiB上限、3回の実確保拒否、解放後の3回の再利用と48回の並行転送が通った。

同じdriverでEngineのnative clip chain、cache再利用、CPU境界と独立C ABIの
GPU専用generatorも実レンダーした。12,467画素のgenerator出力PNGを含む証拠は
``build/swiftshader-native-engine-artifacts-tests/report.json`` に保存した。
8u/32fのNatron GUIでは複数ViewerとEngineが同じ論理deviceを共有し、
画像表示・native入力操作・Validationエラー0・確認ダイアログ0を確認した。
結果は ``build/swiftshader-settled-gui-tests/report.json`` に保存した。
これらはWindowsでのローカル検証であり、CI実行、配布パッケージと
Linux/macOSの検証は別の受入条件として保持する。

更新後のIOでもCPU側14構成とGPU側16構成が通過した。
``build/ocio-failure-raster-tests/report.json`` と
``build/ocio-failure-native-*-tests/report.json`` を再照合した統合結果は
``build/vulkan-ocio-callback-fallback-proof.json`` に保存した。

4K/8Kの描画コア計測は ``build/native-performance/proof.json`` に保存した。
8bit/float、単一画像・A/B wipe・native cache copyの計24条件で、各20フレームを
計測した。floatのcache copyと描画を合わせたp50/p95は、RTXでは4Kが
1.37/1.42 ms、8Kが4.06/4.12 ms、AMDでは4Kが11.24/11.64 ms、8Kが
42.70/48.78 msだった。計測区間のCPU画素転送は0で、実行後のreadbackと
Validationエラー0を確認した。これはValidation有効の独立した描画コア試験であり、
Natronのスケジューラ・WSI表示・Parsec・複数ウィンドウは含まない。
VMAの確保量サンプルは記録したが、ドライバのVRAM予算や瞬間的なピークを示さない。

NatronRendererの4K/8K処理計測
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``tools/test-windows-vulkan-performance.ps1`` はNatronRendererで
4K (3840x2160) / 8K (7680x4320) の画像生成、配布IO bundleの
OCIO CDL変換、分散した画素のPNG出力を実行する。
同じ画像・slope (1.2, 0.8, 1.1) をCPUとnative GPUの両経路に通し、
RTX/AMDとValidation有効/無効の計8回、16グラフ条件を測る。
各条件は3回のwarmupと20回の計測で、記録されるp50/p95は
Natronの同期 ``app.render`` 呼出しから戻るまでの時間である。

プロジェクトとPNGのサイズは64x48であり、生成画像・OCIO入力は4K/8Kである。
非tile generatorとprobeの明示的な入力ROIが全画像処理を要求する。
実際の生成allocation boundsとOCIOからprobeに渡されたclip boundsを
毎回検査し、全画像の縮小proxyを測定してしまうことを防ぐ。
probeは画像全域の端・内部から画素を取り、保存したPNGの各画素を
独立したCPU計算式と照合する。計測とは別のnative full-image readbackで、
RGBAの全画素を同じ式と最大誤差2e-6以内で照合する。

画像cacheは各計測の前に消去し、消去自体を計測時間に含めない。
コンパイル済みplugin programは保持する。PNG writerは両経路ともCPU設定とし、
nativeの画像生成・CDL変換ではCPU画素転送を行わず、PNG境界で64x48 RGBA32Fの
49,152 bytesだけを読み戻す。CPUグラフでは画像転送はない。
この条件は新しい画像をcacheなしで処理する時間を示し、Viewerの再生速度や
cache hit時の表示速度を示さない。Validation無効の値も、GPU時間だけではなく
Natronのスケジューリング・確保/解放・同期・小さなPNG出力を含む。

計測専用の ``NatronOfxVulkanTestSuite`` V2は、V1のABIを維持したまま
共有device全体の完了した画像upload/download回数とtexel bytesを公開する。
Viewerやoverlayの画像転送も含み、行padding、uniform/LUT bufferとGPU内copyは
含めない。関連するcounterはrender完了後の静止点で比較する。
通常のtest profileはValidation有効で、profile内だけの
``NATRON_TEST_VULKAN_VALIDATION=0`` によって本番相当の計測を許可する。
API呼出しや ``NATRON_VULKAN_VALIDATION=1`` の明示指定は優先される。

VMAのdevice-local memory block確保/解放callbackによって、論理deviceの
全寿命の管理対象blockピークも取得する。これはswapchainやdriver固有の
確保を含まない。AMDの統合GPUではdevice-local heapが共有メモリを含むため、
専用VRAMだけの使用量とは扱わない。``VK_EXT_memory_budget`` のusage/budgetも
render前後に取得するが、usageは現在のprocessの推定値であり、サンプル間の
瞬間的なピークを保証しない。budgetは他のsystem activityに影響される。
定義は `Vulkan memory budget specification
<https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html>`_
と `VMA statistics
<https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/statistics.html>`_ に従う。

実行報告・PNG・source/binary hashは
``build/viewer-presentation-headless-performance-tests/report.json`` に保存する。
``tools/verify-vulkan-performance.py`` が現在のsource、実行ファイル、PE import、
PNG画素、counter差分、p50/p95、メモリピーク、test profileの開始/EOF終了logと
ユーザー設定の不変性を再照合する。統合結果は
``build/vulkan-application-performance-proof.json``、16条件の表は同名CSVである。
この証拠でもWSI、複数Viewer、playback/scrubbing、Parsec、旧GLとの比較と
driver全体のVRAMピークは未検証として保持する。

計測API追加後のdevice-loss回帰は
``build/viewer-presentation-loss-hardware-tests/report.json`` と
``build/viewer-presentation-loss-software-tests/report.json`` に保持する。
15 host構成、3 core recovery構成、6,045,696画素、737 source filesと
332 PE filesを現在のbuildで再照合した。profileによるValidation既定値と
V1 test ABIの維持もこれらの回帰で確認した。
続くメモリ逼迫の15構成も再検証し、接続編集後に両Viewerへ明示的に
描画要求を出した12構成の並行CPU再試行を確認した。以前の暗黙の接続通知では、
ソフトウェアVulkanのscale 2 / 32fで一方の要求が取り消され、barrier待機が
失敗した。失敗の記録は保持し、並行性の判定条件は緩めていない。
現在の結果は ``build/viewer-presentation-concurrent-hardware-tests/report.json``
と ``build/viewer-presentation-concurrent-software-tests/report.json`` に保存する。
``tools/verify-vulkan-application-integration.py`` は性能・復旧・逼迫の証拠が
同じsource/runtime auditを参照することと、現在のsource/binary hashを照合する。
統合結果の ``build/vulkan-application-integration-proof.json`` は、これらの実行と
以下のViewer計測を照合する。完全移行の未完了条件は保持する。

Viewer計測追加前のValidation無効のcold-cache結果は次のとおりである。
対応する証拠は ``build/application-performance-integration-baseline/snapshot.zip``
に保存している。時間はms、管理対象の
device-local blockピークはMiBである。4KとAMDの8Kではnative経路の方が遅く、
全条件の速度向上を示す結果とは扱わない。RTXの8Kではnative経路が短かった。

.. list-table:: NatronRendererの全画像OCIO処理（cache消去は計測外）
   :header-rows: 1
   :widths: 18 12 24 24 22

   * - GPU
     - 画像
     - CPU p50 / p95
     - native p50 / p95
     - native管理対象ピーク
   * - RTX 5080
     - 4K
     - 97.19 / 118.35
     - 127.75 / 139.34
     - 542.00
   * - RTX 5080
     - 8K
     - 337.27 / 378.95
     - 223.51 / 230.13
     - 2072.00
   * - AMD Radeon
     - 4K
     - 89.40 / 98.13
     - 171.18 / 187.58
     - 544.11
   * - AMD Radeon
     - 8K
     - 343.38 / 383.59
     - 458.07 / 497.43
     - 2079.97

実Viewerのフレーム移動・表示計測
----------------------------------------------

``tools/test-windows-vulkan-viewer-performance.ps1`` はNatron本体で
full-resolutionの4K/8K画像生成ノード、配布版OCIO CDL、1個または2個の
独立したViewerを動かす。表示面は物理960x540であり、Viewerが要求する
mipmapは4Kで2、8Kで3となる。元画像は全解像度で生成し、縮小後の画像に
OCIOと表示補正を適用する。NatronRendererの全解像度OCIO計測とは処理範囲が異なる。
各フレームのEngine cache消去は計測外で行う。

profile限定の ``framePresented`` 通知は表示フレーム番号、元画像のformat/RoD、
texture bounds、mipmap、viewport、storage、device generationを記録する。
Vulkanでは成功したpresentとqueue待機の完了、RasterではQPainterの描画完了を
通知する。実際のscanoutやリモート端末への表示完了を保証する通知ではない。
すべてのViewerに新しいフレームが到達するまでを1標本とし、計測中には
画素の読み戻しを行わない。画像保存はframe 2/24の計測後だけに行う。

通常動作のValidation無効matrixはRTX/AMD、4K/8K、8u/32f、Viewer 1個/2個、
native処理/CPU処理とVulkan表示/CPU処理とRaster表示の48構成である。
Validation有効では両GPU、両画像サイズ、両storage、Viewer 2個、native/CPUの
16構成を検証する。各構成は3 warmupと20標本を持つ。
報告は ``build/vulkan-viewer-performance-settled-production-tests/report.json``
と ``build/vulkan-viewer-performance-settled-validation-tests/report.json`` に保存する。

``tools/verify-vulkan-viewer-performance.py`` は、現在のsource/binary hash、
PE import、frame metadata、転送counter差分、p50/p95、終了logとユーザー設定を
再照合する。全保存画素を画像生成式・box縮小・表示補正・補間から独立に計算した
参照値と比較し、native/CPU/Rasterの保存画像同士も照合する。
32fの許容誤差は1階調値、byte表示はLUT丸め・error diffusionと補間を含め2階調値である。
Vulkan native経路の計測区間は画素upload/downloadともゼロ、CPUからVulkanへの
経路は各Viewerに縮小後のfloat画像を1回upload、Raster経路は転送ゼロを要求する。
証拠は ``build/vulkan-viewer-performance-proof.json`` と同名CSVである。

Validation無効・32f・独立したViewer 2個のcold-cache結果は次のとおりである。
時間は新しいtimeline frameの要求から両Viewerの通知までのmsであり、連続再生の
frame rateとは扱わない。Vulkan native経路は8Kで短かったが、AMDの4Kでは
CPU経路と同程度であった。

.. list-table:: Natron Viewerのcold-cacheフレーム移動（960x540の表示面を2個）
   :header-rows: 1
   :widths: 16 12 24 24 24

   * - GPU
     - 元画像
     - native p50 / p95
     - CPU＋Vulkan p50 / p95
     - CPU＋Raster p50 / p95
   * - RTX 5080
     - 4K
     - 116.15 / 124.22
     - 152.28 / 176.65
     - 183.31 / 202.53
   * - RTX 5080
     - 8K
     - 188.05 / 193.31
     - 536.30 / 579.28
     - 553.84 / 586.75
   * - AMD Radeon
     - 4K
     - 158.04 / 163.34
     - 153.70 / 177.63
     - 178.35 / 201.13
   * - AMD Radeon
     - 8K
     - 376.52 / 382.52
     - 546.00 / 613.43
     - 551.46 / 613.43

管理対象VMA blockピークとdriver usageのサンプル最大値はCSVに保持する。
どちらもdriver全体の瞬間的なVRAMピークとして扱わない。測定時のprimary表示は
NVIDIA adapterに接続されており、AMDのViewerも同じ物理desktopへpresentする。
この結果はそのローカル表示構成での値である。

この検証で、計測用OFXノードが出力formatを既定のHDのまま保持する問題を直し、
width/heightをclip-preferences依存paramとして宣言した。元解像度を必ず生成する
ノードは半解像度RoDの問い合わせを失敗として返し、Hostにrender-scale非対応を伝える。
Viewerをぴったり表示した場合の右端・下端1画素の欠落も、表示領域のexclusive境界を
widget外周へ修正して解消した。参照画素の照合はこれらの境界も含む。

GUIのPython ``print`` はScript Editorへ転送されるため、計測中のphase logは
専用profileの ``progress.json`` に保存する。終了markerはprocessのstdoutへ
直接出力し、終了とcapture処理はpresent通知を返した後のQt eventで行う。
起動・終了の確認ダイアログは専用設定とmodal監視、process watchdogで検出し、
既存のNatronセッションやユーザー設定を変更しない。

``tools/verify-vulkan-application-integration.py`` の
``--viewer-performance-proof`` は、この64構成・1280標本を処理・復旧・逼迫の
同一buildの証拠に加える。cold-cacheでの制御されたtimeline移動は確認できるが、
このcheckpointでは連続playback、実scanout/Parsec、driver全体のVRAMピーク、
旧GLとの性能比較は未検証だった。連続playbackの機能検証を、後述のWindows
scheduler matrixとして追加した。
このcheckpointではViewer 64構成・1280標本・107,827,200保存画素と、
NatronRenderer 8構成・16 graph・320標本、15 host recovery構成、
15 memory-pressure構成を同じ737 source filesと332 PE filesに照合した。
合計301,856,488画素を検証し、ユーザー設定のfingerprintは変更されていない。
Coreはhardware 20実行とSwiftShader 9実行も通過した。
``build/viewer-presentation-integration-baseline/snapshot.zip`` は所有するsource、
選択した実行ファイルと証拠を保存する。依存関係を含む単独配布パッケージではない。

Windowsの連続再生・停止・再開
------------------------------------------------

``tools/test-windows-vulkan-playback.ps1`` は実際のNatron playback schedulerを
使用する。RTX/AMD、4K/8K原画像、8u/32f Viewer storage、Vulkan/Raster表示の
16構成を、2個の表示中Viewerで検証した。7段階・計112操作は順再生、逆再生、
ループ、往復再生、途中停止、もう一方のViewerからの再開、そのViewerの逆再生である。
両Viewerのframe通知を記録し、停止後の表示時刻とtimelineが変化しないことを確認する。
途中停止はもう一方の描画を中断できるため、全frameの同時表示を条件とはしない。
両方の最後に表示したframeを、その表示時刻のCPU参照と照合する。

要求frame rateは8 fpsである。これは連続scheduler・表示・cancelの機能回帰であり、
実際の再生速度、physical scanoutまたはParsecの性能測定ではない。
2 Viewerから各段階で取得した計224 PNGは、画素全体を独立した手続き画像・縮小・
表示変換の参照に照合する。native / RasterのValidationエラーは0、利用者設定の
fingerprintは変更されていない。

起動は ``tools/run-windows-owned-gui-test.ps1`` が子process専用の環境・INI・
cache・markerを与え、所有するPIDだけを監視する。起動を非表示にすると、最初の
native windowがQtからvisibleと見えてもWSI surfaceが未露出のままになることがある。
テスト用windowを明示的にhide/showして露出させ、表示完了から検証を始める。
通常起動の設定や確認ダイアログの動作は変更しない。

結果は ``build/windows-vulkan-playback-settled-tests/report.json`` に保存する。
``tools/verify-vulkan-playback.py`` が入力hash、全画素、Validation・終了logと
専用profileを再照合し、 ``build/vulkan-windows-playback-proof.json`` にまとめる。
統合verifierの ``--playback-proof`` は同じsource・PE監査と設定fingerprintを要求し、
Windows証拠にこのmatrixを加える。他OSの検証は完了条件に含めない。

.. code-block:: powershell

   tools/test-windows-vulkan-playback.ps1 -ReportDirectory build/windows-vulkan-playback-settled-tests
   uv run --frozen python tools/verify-vulkan-playback.py --report build/windows-vulkan-playback-settled-tests/report.json


移行の目的と範囲
----------------

Natron本体と配布するOFXプラグインからOpenGLの実行時依存をなくし、
GPUによる表示・画像処理をVulkanに統一する。CPUによるOFX処理、
Qt Widgetsの通常の描画、既存プロジェクトとPython APIは維持する。
「完全移行」は、すべてのOFXアルゴリズムをGPU化する意味ではない。

Windowsを先行対象とし、LinuxのX11/Wayland、macOSのMoltenVKを順に検証する。
Windows版の完成と全プラットフォームでの完成を別々に判定する。
macOSではVulkanをMetal上で動かす。OpenGLの非推奨化はmacOS 10.14で
明示されているが、全OSに共通する廃止期限があるという前提にはしない。

最終構成ではOpenGLのフォールバック、OSMesa、GL/Vulkan相互運用層を
常設しない。Vulkanを使用できない場合はCPU処理とラスタ表示に切り替え、
GPU専用ノードは理由を表示して処理を停止する。

移行開始時の依存関係（履歴）
------------------------------------

以下は移行開始時のリポジトリとローカルビルドを調査した結果である。

.. list-table:: 移行対象
   :header-rows: 1
   :widths: 20 40 40

   * - 領域
     - 現在の主な実装
     - 移行先
   * - Viewer
     - ``Gui/ViewerGL*``、``Engine/OpenGLViewerI.h``、
       ``Engine/ViewerInstance*``、``Gui/ViewerVulkan.cpp``
     - Vulkanによる画像表示、表示変換、比較、文字・オーバーレイ描画。
       EngineとGuiの境界を描画APIに依存しないインターフェースにする。
   * - 編集用の描画
     - ``Gui/CurveWidget*``、``Gui/Histogram*``、
       ``Gui/HostOverlay.cpp``、``Engine/RotoPaintInteract.cpp``、
       ``Engine/TrackerNodeInteract.cpp``
     - カーブ・ヒストグラムはQWidget/QPainterを第一候補とし、性能不足なら
       Vulkanを使う。Roto/Trackerは共通のオーバーレイ描画命令へ移す。
   * - GPU画像処理
     - ``Engine/Image*``、``Engine/Texture*``、``Engine/GLShader*``、
       ``Engine/GPUContextPool*``、``Engine/OSGLContext*``、
       ``Engine/EffectInstanceRenderRoI.cpp``
     - Vulkan画像、転送、Compute Shader、明示的な同期・寿命管理。
   * - OFXホスト
     - ``Engine/OfxHost.cpp``、``Engine/OfxClipInstance.cpp``、
       ``Engine/OfxEffectInstance.cpp``、``Engine/OfxOverlayInteract.cpp``、
       ``Gui/CustomParamInteract.cpp``、``libs/OpenFX/HostSupport``
     - CPU画像の標準OFX経路を維持。OFX 1.5のDrawSuiteと
       OverlayInteractV2を実装し、OpenGL Render Suiteを廃止する。
   * - プラグイン・ビルド・検証
     - ``build-configs/windows-ofx``、Qt OpenGLWidgets、GLAD、
       OS別GLコンテキスト、``Tests/OSGLContext_Test.cpp``、GUI smoke
     - OFXの再移植・再構築、Vulkan依存、GPU検証、配布物の依存監査。

特定のGL関連識別子による検索だけでも66ファイルに依存が見つかった。
これは完全な棚卸し件数ではなく、初期調査の下限である。
カーブ・Roto等には ``glBegin`` による即時描画があり、Widgetの基底クラスを
置き換えるだけでは移行できない。

ローカルで再構築したMiscとArenaには ``OPENGL32.dll`` の直接インポートがある。
IOは前回の作業で既存バイナリを使っており、今回の完全移行ではソースからの
再構築と依存監査も対象にする。CPUレンダリングできることと、DLLがOpenGLに
依存しないことは別々に確認する。

``tools/audit-ofx-vulkan.ps1`` でテスト用OFXキャッシュと実バイナリを監査した。
579のキャッシュ登録バージョン、556の一意なOFX IDを一覧にした。
Natronが表示する572ノードには組み込みノード等も含まれるため、件数を区別する。

.. list-table:: 2026-10-11の配布バンドル直接依存
   :header-rows: 1
   :widths: 40 30 30

   * - バンドル
     - キャッシュ登録バージョン数
     - OPENGL32.dllの直接インポート
   * - Arena
     - 26
     - あり
   * - CImg
     - 34
     - なし
   * - GMIC
     - 360
     - なし
   * - IO
     - 23
     - あり
   * - Misc
     - 136
     - あり

5つのIO OCIOノードがOpenGLレンダリングを ``true`` と宣言し、
``net.sf.openfx.Shadertoy`` は ``needed`` と宣言していた。
それ以外の573登録バージョンの宣言は ``false`` だった。
これをCPUレンダリングやDrawSuite対応が検証済みという意味にはしない。
特にShadertoyはGPU専用として移植する必要がある。
監査時点ではHostSupportとローカルのOFXソースにDrawSuite/OverlayInteractV2の
実装がなかった。ホスト実装は後述のとおり追加したが、配布プラグインは引き続き移植対象である。

一覧は ``build/ofx-vulkan-audit/plugins.csv``、モジュールのSHA-256と直接DLL依存は
``report.json`` に保存する。CPU描画とオーバーレイの判定は未検証として残す。
推移依存と実行時ロードの監査は別途必要である。
IOの既存バイナリはdumpbinで読み取れなかったため、uvで管理する
``pefile==2024.8.26`` を使ってPE import directoryを読み、同じ監査に含めた。

確認したPCにはRTX 5080、AMD内蔵GPU、Parsec仮想ディスプレイがある。
Qt 6.11.2のSDKはVulkan対応である。2026-10-11にインストールされた
``C:\VulkanSDK\1.4.363.0`` を明示指定してGPU機能照会とシェーダツールを検証した。

.. list-table:: ローカルの機能照会結果
   :header-rows: 1
   :widths: 40 30 30

   * - 項目
     - NVIDIA GeForce RTX 5080
     - AMD Radeon(TM) Graphics
   * - デバイスAPI
     - Vulkan 1.4.351
     - Vulkan 1.4.315
   * - timelineSemaphore / synchronization2 / dynamicRendering
     - すべて対応
     - すべて対応
   * - RGBA8/16 UNORM、RGBA16F/32Fのsampled/storage/transfer
     - すべて対応
     - すべて対応
   * - graphics + compute + Win32 presentのキューファミリー
     - 0、16キュー
     - 0、8キュー
   * - 2D画像の最大寸法
     - 32768
     - 16384

LoaderのインスタンスAPIは1.4.363、Validation Layerは取得可能だった。
glslang 16.6.0でCompute ShaderをVulkan 1.3向けSPIR-Vへコンパイルし、
SDK同梱のSPIR-V Toolsで検証した。両GPUは暫定最低機能を満たし、最初の試作は
RTX 5080を優先する。Win32サーフェスの照会は成功しているが、Qt Viewerでの
以下の独立テストで、表示とGPU Computeの実行検証も完了した。

再取得は次のコマンドを使う。機器固有のレポートはGit管理外の
``build/vulkan-probe/report.json`` とGPU別のVulkan Profiles JSONに保存する。

.. code-block:: powershell

   .\tools\probe-windows-vulkan.ps1 -SdkRoot C:\VulkanSDK\1.4.363.0

Codexの起動時プロセスには旧SDK 1.4.335.0の環境変数が残っていたため、
このツールは実行中だけSDKパスを切り替える。OSの環境変数を変更しない。
照会ログにはOBSの旧APIフックとVK_KHR_displayのICD警告が記録された。
後者はWin32サーフェスの照会失敗ではない。表示試作で外部レイヤーの影響を
切り分ける。

推奨アーキテクチャ
------------------

**Engineが所有するVulkanデバイスを、画像処理とViewerで共有する方式を推奨する。**
新しいクラス名は仮称とする。

* ``GpuDevice`` はデバイス、キュー、メモリアロケータ、機能照会を所有する。
  CPU専用のNatronRendererでは遅延初期化し、GPU・表示サーフェスを必須にしない。
* ``GpuImage`` は ``VkImage``、フォーマット、領域、レイアウト、処理完了値を
  保持する。CPU画像とGPU画像のどちらが最新かを追跡し、必要な境界でだけ転送する。
* ``GpuScheduler`` はワーカごとのコマンドプール、キュー送信の排他、
  timeline semaphore、完了後の破棄を管理する。GLのスレッドローカルな
  current contextをそのまま置き換える設計にはしない。
* Guiは ``QWindow`` と ``QVulkanInstance`` で表示サーフェスを作り、
  同じデバイス上にViewerごとのswapchainを持つ。
  ``QVulkanInstance::setVkInstance`` で共有インスタンスを利用する。
  EngineがGuiのヘッダを参照する構造は導入しない。
* ``OverlayDrawList`` は線、三角形、点、文字、座標変換を保持する。
  ホストのRoto/TrackerとOFX DrawSuiteが共通の描画先を使う。
  太線・点はジオメトリ化し、必須ではないVulkanのwideLines機能に依存しない。

Qt Widgetsのドック、メニュー、ノードグラフは維持する。
``QWidget::createWindowContainer`` には重なり順、フォーカス、
``QWidget::render`` との統合に制約があるため、最初の試作で確認する。
画像上の文字や操作ハンドルはViewer内部で描き、透過QWidgetの重ね合わせを
前提にしない。ViewerのスクリーンショットはVulkanからのreadbackで取得する。

代替候補はVulkanを明示指定した ``QRhiWidget`` である。
Widgetとしての統合は容易だが、QRhiにはQtの版を跨ぐソース・ABI保証がなく、
デバイス所有権とEngineからの利用を慎重に扱う必要がある。
最初の試作でネイティブWindow方式に重大な統合問題があれば比較する。
``QVulkanWindow`` は独立した表示試作には使えるが、自身がデバイスを管理する
ため、そのまま最終構成の共有デバイス設計とはしない。

APIと依存関係
-------------

* 開発用SDK/ツールは検証したVulkan SDK 1.4.363.0を固定し、実行時の暫定最低要件は
  Vulkan 1.3とする。他の対応機器とMoltenVKを検証して最終的な最低要件を確定する。
  SDKの版と実行時デバイス要件を混同しない。
* timeline semaphore、synchronization2、dynamic rendering、必要な画像形式の
  sampled/storage/transfer対応を明示照会する。RGBA32Fを計算精度の基準とし、
  RGBA16F、8/16bit画像への変換はCPUとの一致を検証してから使う。
* Loader/Headers、Validation Layers、SPIR-V Tools、glslang、
  Vulkan Memory AllocatorをCMakeで管理し、版・取得元・ライセンスを記録する。
  通常の組み込みシェーダはビルド時にSPIR-Vへ変換する。
* 色変換をGPU化する場合はOpenColorIO 2.5以降のVulkan用GLSL生成を利用する。
  現在Arenaで使うOCIO 2.6と同系列をMSVC側にも導入し、GCC製C++ライブラリを
  MSVCホストへ直接リンクしない。既存の表示変換をCPU基準画像と比較する。
* macOSはMoltenVKとportability拡張を明示的に検証する。MoltenVKがVulkanの
  一部をMetalへ変換するため、Windowsでの動作をそのまま対応保証にしない。

OFX互換性の方針
---------------

現行のOFX GPU仕様はOpenGL/OpenCL/CUDA/Metalを定義し、Vulkanは将来の
追加候補として記述されている。標準OFXのGLテクスチャに ``VkImage`` を
渡したり、Vulkan対応を標準機能として宣言したりしない。

.. list-table:: 最終構成での処理
   :header-rows: 1
   :widths: 30 70

   * - プラグインの種類
     - 方針
   * - 標準CPU OFX / G'MIC / CImg
     - CPU経路を維持する。Vulkan Viewerへのアップロードで利用可能にし、
       全ノードのGPU化を約束しない。入力がGPUにしかない場合は必要なROIを
       CPUへ戻し、連続CPUノード間で再転送しない。
   * - CPUとOpenGLの両方を実装
     - OpenGL対応を無効として通知しCPUで実行する。
       配布するソースについてはGLのリンク・コード・旧描画も除去する。
   * - OverlayInteractV2 + DrawSuite
     - Vulkanの共通描画先へ接続する。プラグインの描画と入力イベント、
       DPR、画像座標、Undoをまとめて検証する。
   * - OpenGLの旧OverlayInteractV1
     - 自動変換しない。配布プラグインのSupport/SupportExtと個別実装を
       DrawSuiteへ移植する。未対応の外部プラグインは旧interactを呼び出さず、
       通常のパラメータUIで操作する。カスタムUIのみの機能は制限を明示する。
   * - Shadertoy等のOpenGL依存ノード
     - Vulkan/SPIR-Vへ個別移植する。任意GLSL、複数パス、サンプラ、
       iTime/iFrame/iMouse等を調査する。移植できないものは利用不可と表示し、
       プロジェクト中のID・パラメータ・接続は保存する。OSMesaでの代替はしない。
   * - クローズドソースのOpenGL専用OFX
     - 完全互換は保証しない。Vulkan版またはCPU版を必要とする。
       OpenGLライブラリに依存するバイナリはOpenGL不要の配布構成に含めない。

VulkanによるOFX GPU実行は、必要になった段階でバージョン付きのNatron独自
拡張として別途設計する。共有デバイス、画像の所有権・レイアウト、同期、
キャンセル、ROIを仕様化し、ホストと対象プラグインを同時に更新する。
これはCPU OFX互換の維持やVulkan Viewerの実装開始を妨げる前提条件にしない。
上流への提案・連絡は別の作業として扱う。

既存プロジェクトのノードID・パラメータと公開Python APIを移行の基準にする。
GPUバックエンド名やGLテクスチャハンドルをプロジェクトの意味に含めない。
旧GPU設定を新しい能力・実行設定へ対応付け、保存形式の変更が必要なら
バージョン付きの読込処理を追加する。非対応ノードを削除・置換してから
保存する挙動は導入しない。

移行段階と完了条件
------------------

各段階を小さなレビュー単位に分け、前段階の完了条件を満たしてから進める。
途中の比較用GL構成は期限付きで残せるが、最終配布の選択肢にはしない。

0. **依存棚卸しと設計の試作**

   全GL呼び出し、プラグインのGPU/CPU能力、旧interact、DLLと遅延ロードを
   一覧化する。現在の29ネイティブテスト、7種のOFX実レンダリング、
   GUIの572ノード登録を基準として保存する。G'MICは360ノードが現行基準。
   ノード数だけでなくID・機能単位で差分を追跡する。
   4Kの浮動小数点画像をQtのドック内に表示する試作と機能照会を行う。
   RTX/AMDを明示選択し、複数Viewer、ドック着脱、DPI、ポップアップの
   表示を確認する。

   完了条件: GPU機能レポート、プラグイン互換表、表示方式の決定、
   CPU/GL基準画像・性能測定、実装単位と依存順序が揃う。

1. **Vulkan基盤とAPIに依存しない境界**

   CMake依存とシェーダビルドを追加する。デバイス、画像、転送、同期、
   メモリ上限、完了後の破棄を実装する。Viewer/Overlayのインターフェースから
   GLコンテキスト操作を分離する。CPU専用Rendererの起動経路を維持する。

   完了条件: GUIなしでupload/compute/readbackが成功し、Validation Layersで
   同期・寿命のエラーがない。CPU専用起動でGPUの初期化を要求しない。

2. **Viewerと編集操作の移行**

   CPU画像からVulkan Viewerへ接続し、ズーム、パン、ROI、mipmap、チャンネル、
   A/B・wipe、露出、表示変換、色拾い、文字、複数viewを移植する。
   Roto/Trackerとホストの操作ハンドルを共通描画へ移し、Curve/Histogramの
   GL描画を除去する。OFXの旧interactを新Viewer上で呼び出さない。

   完了条件: 現行Viewerの機能チェックリスト、画素比較、入力・Undo・
   ドック/DPI検証が通る。未対応OFXハンドルの一覧とUI上の説明が揃う。
   この段階だけではGPU画像処理やOFX全体の完全移行を達成したと宣言しない。

3. **エンジンのGPU画像処理とキャッシュの移行**

   fill/copy/channel/mask/mix等のGL処理をCompute Shaderへ移す。
   renderRoI、キャッシュ、CPU/GPU境界、複数スレッド、キャンセル、
   VRAM不足・device lostを統合する。GPU画像をViewerまで同一デバイスで渡す。
   CPU専用OFXにはCPU画像を渡す契約を維持する。

   完了条件: CPUとの差が定義した許容誤差以内。GPU内で完結する経路に
   不要なCPU往復がなく、キャッシュ破棄・中断・終了時の寿命が安全。
   VRAM不足からCPUへ再試行でき、GPU専用処理では明確なエラーを返す。

4. **OFX DrawSuiteと配布プラグインの移植**

   OpenFX SDK/HostSupportとSupportExtを、Natron拡張を保ったまま更新する。
   DrawSuite/OverlayInteractV2を実装し、Transform、CornerPin、Crop等の
   操作ハンドルから順に移植する。Misc/CImg/Arena/G'MICに加えIOも再構築し、
   GLリンク・旧interact・カスタムUIを監査する。Shadertoyは独立した移植単位に
   する。GL専用ノードは移植完了または明示された非対応として扱う。

   完了条件: CPUレンダリング、新オーバーレイ、旧プロジェクト読込・保存、
   Unicode、各バンドルの依存監査が通る。すべての配布ノードの対応表が完成し、
   意図せず消えたノードや黒い結果を成功扱いする経路がない。

5. **OpenGL削除と配布検証**

   GLAD、GLShader、OSGLContext、旧GPUContextPool、QOpenGLWidget、
   GL Render SuiteとGL専用設定・テスト・ビルド依存を削除する。
   ソース検索、直接/推移DLL依存、実行中モジュールを監査する。
   Qt/PySideや画像ライブラリがGLをロードする場合は機能を無効にして再構築する。
   CPUだけの起動・レンダリングとVulkanなしのラスタViewerも検証する。
   今回はWindowsを対象にする。Linux X11/Wayland、macOS MoltenVKでの
   同条件の確認は、別のplatform検証として扱う。

   完了条件: WindowsのNatron本体と配布プラグインがOpenGLを要求・ロードせず、
   Viewer/GPU処理はVulkanを使い、CPU経路も通る。外部OFXの非対応条件を文書化し、
   配布パッケージの再現可能なビルドと実機検証が揃う。

検証方法
--------

* 画像: 8/16bit整数・float、負値/HDR、premultiply、NaN/Inf、異なるPAR、
  非ゼロ画像原点、ROI/tile/mipmap、色変換をCPU基準と比較する。
  単純コピー等は完全一致、float演算や色変換は事前に決めた誤差で判定する。
* 操作: Roto/Tracker、Transform/CornerPin/Crop、ドラッグ、筆圧、Undo、
  カスタムパラメータ、複数Viewer、ドック着脱、DPI、色拾いを確認する。
* 安定性: scrubbing、キャンセル、同時render、終了、GPU選択、VRAM制限、
  device lost/再初期化、headless CPU、表示サーフェスなしのGPU処理を確認する。
* 性能: 同じ画像・設定で4K/8K再生、scrubbingのp50/p95、CPU/GPU転送量、
  VRAMピーク、複数Viewerを測定する。CPU OFX主体のグラフとGPU内完結グラフを
  分け、基準測定後に許容する劣化幅を確定する。
* CI: ネイティブテスト、SPIR-V検証、CPU OFX、ソフトウェアVulkanでの機能検証を
  自動化し、RTX/AMDおよび他OSの実機で表示と性能を別途検証する。
  Pythonによる検証・依存管理は引き続き ``uv`` を使用する。

GUIテストの確認ダイアログ
--------------------------

起動時の確認と終了時の保存確認がテストを止めることがある。
2026-10-11にユーザーから指摘された条件として、表示試作とNatron GUIの
自動テストに以下を適用する。通常利用時の確認動作はテストのために変更しない。

* 起動時には更新確認、外観、ショートカット、自動保存復元等の確認があり得る。
  ``--no-settings`` だけで確認がすべてなくなるとは扱わない。
  ``Gui/GuiAppInstance.cpp`` の初期化では、テスト用Pythonの実行前にも
  確認があるため、Python内のQTimerだけをタイムアウト対策にしない。
* テストで起動したプロセスに外部からの起動・実行・終了の待機期限を設ける。
  予期しないダイアログは種類・タイトル・可能なら画像を記録して失敗にする。
  一律のEnter、Escape、「いいえ」で閉じる操作は行わない。
* 専用プロジェクトとテスト用の保存先を使う。既存の作業プロジェクトを変更せず、
  設定・自動保存の分離方法を確定してから無人テストを実行する。
  自動保存復元の「いいえ」またはEscapeは ``Project::clearAutoSavesDir`` を
  呼ぶため、ユーザーの自動保存を消して起動ダイアログを回避しない。
* 画像・操作のsmokeでは結果を書き終えてからテストプロセスを明示的に終了する。
  現行GUI smokeは ``QApplication.exit`` を使用している。
  この終了を、``Gui::closeEvent`` からの保存確認を含む通常終了の検証とは
  区別する。通常終了の保存・破棄・キャンセルは専用プロジェクトで別に確認する。
* 終了が止まった場合はログを保存し、そのテストが起動したPIDのみを対象にする。
  実行中のNatronを名前でまとめて終了しない。結果JSONが書かれていても、
  予期しないモーダルや終了失敗があればテスト全体を成功扱いしない。

``GuiSmoke`` / ``OfxGuiSmoke`` は、新しい ``build/test-profiles`` 配下の
プロファイルでINI設定、キャッシュ、temp、自動保存を分離する。
``NATRON_TEST_PROFILE`` を指定したプロセスだけで有効になる。
``Global/AppSettings.h`` はINI形式を明示する。組織名を渡すQSettingsの
コンストラクタには ``setDefaultFormat`` が適用されないためである。
テスト設定の更新確認とOCIO起動確認を無効にし、通常のユーザー設定には書き込まない。
``--no-settings`` だけに依存しない。
起動したPIDを外部監視し、時間切れの段階とログを保存する。
Pythonの検査時には予期しないモーダルがないことも確認する。

2026-10-11に両GUI smokeの成功と、実行前後のユーザー設定レジストリが
変化していないことを確認した。通常終了の保存確認テストはまだ残っている。

Vulkan基盤の実装と実機検証
------------------------------------

``Gpu/NatronGpu`` はQt/Guiに依存しないデバイス、メモリ、画像転送、
Compute Shaderの基盤である。Vulkan 1.3、timeline semaphore、Synchronization2を
使用する。Volkのインスタンス/デバイス別関数テーブルを使い、Vulkan loaderを
動的に読み込む。VMA 3.4.0とVolk header version 363はSDKから取得し、
MITライセンスを同梱した。

``Gpu/NatronGpuQt`` は共有インスタンスをQtに渡し、QWindowのサーフェスと
自前のスワップチェーンを使う。複数のQtコンテナが、Computeと同じ論理デバイスと
画像を共有する。表示画像をVulkanからreadbackする経路も実装した。
画像のblit表示と、後述のViewer Rendererによるビューポート描画に対応した。
既存Viewerの機能をすべて置き換えるものではない。

2026-10-11にRTX 5080とAMD Radeon Graphicsで、以下を確認した。

* RGBA8、RGBA16整数、RGBA16F、RGBA32Fのupload/downloadはビット単位で一致した。
* 負値、HDR、独立したalpha、257x129の端数サイズを含む2段のComputeを
  CPU参照と比較した。最大絶対誤差は両GPUで ``9.53674e-7`` だった。
  4つのホストスレッドによる共有デバイスへの同時呼び出しも通った。
* 3840x2160 RGBA32F画像とGPUで反転した画像を2つのQtコンテナに表示し、
  スワップチェーンから取得した実フレームのRGBA値をCPU参照と比較した。
  リサイズ、ドックの切り離し・再接続、DPR 1.0 / 1.5を確認した。
* KHRONOS Validation LayerとSynchronization Validationを有効にし、
  リソース解放まで含めてエラー数0だった。
* Computeテストでは両GPUともOpenGL DLLを読み込まなかった。
  表示テストではAMD側は読み込まなかった。NVIDIA側では
  ``vkCreateSwapchainKHR`` 中のドライバー ``nvoglv64.dll`` が
  ``opengl32.dll`` を読み込むことをDLL通知の呼び出し履歴で確認した。
  暗黙レイヤーを無効にしても再現した。アプリのOpenGL呼び出し・リンク依存と、
  このドライバー内部の動作を区別して監査する。

再実行のコマンドは以下のとおり。SDKとQtを明示指定できる。

.. code-block:: powershell

   .\tools\build-windows-vulkan.ps1
   .\tools\test-windows-vulkan.ps1

レポートは ``build/vulkan-tests/gpu0`` / ``gpu1``、DPR 1.5の結果は
``build/vulkan-tests-dpi150`` に保存した。外部の監視は専用PIDだけを対象にする。
単体の表示試作なので、Natronの起動設定や保存確認ダイアログは実行しない。

この基盤はまだ同期実行であり、非同期スケジューラ、キャンセル、device lost、
CPUへのフォールバック、VRAM制限とキャッシュの統合は残っている。
本体のViewerはVulkan表示へ接続した。オーバーレイ、Image/GPU処理、
OFXのOpenGL経路はまだ移植中である。
この検証をVulkan完全移行の完了とは扱わない。

Viewer renderer and upload boundary
-----------------------------------

``Engine/ViewerFrameTexture.h`` のアップロード結果はバックエンド所有の
不透明なオブジェクトになった。``ViewerInstance::updateViewer`` はGLテクスチャ型を
使わず、OpenGLコンテキストの切り替えも呼ばない。本体Viewerのアップロードも
``ViewerFrame`` とVulkan staging bufferへ置き換えた。
オーバーレイ側のGLインターフェースは引き続き移植対象である。

``Gpu/ViewerFrame`` は、mipmap後のピクセル境界を持つキャッシュタイルを受け取る。
非ゼロの画像原点を保持し、8bit BGRAはRGBA8へチャンネル順だけを変換し、
float RGBAは値を変更せず転送する。未転送の領域はGPU上でゼロ初期化する。
``VulkanImage::uploadRegion`` は画像内の部分更新とパディングされた行に対応した。

``Gpu/ViewerRenderer`` はcanonical座標のビューポート、画像境界、ROI、formatを
受け取り、物理ピクセル寸法のRGBA32FターゲットをCompute Shaderで生成する。
画像の行0は下、出力の行0は上とする。nearestとbilinear、gain/offset、
sRGB/Linear/Rec.709/BT.1886、gamma、checkerboard、premultiplied/straight/opaque、
4種類のwipeと4種類のstack比較に対応した。CPUで表示変換済みの8bit画像は
変換をもう一度かけない。``VulkanCanvas::setFrameRenderer`` を使い、
スワップチェーンの物理寸法で再描画してから表示する。

2026-10-11に両GPUで以下を確認した。

* 部分転送は4つの整数/float形式でビット一致した。最終行・列、画像内のオフセット、
  行パディングと、更新対象外のピクセルの保持を含む。
* BGRA8/RGBA32Fのキャッシュタイルは、逆順の転送、1ピクセルの再描画、
  非ゼロ原点、未転送領域、floatのNaN payload等を含めてビット一致した。
* Rendererの92フレーム、3552チャンネルをCPU期待値と比較した。
  許容誤差は ``2e-5 * max(1, abs(expected))`` とし、最大絶対誤差は
  RTXで ``7.62939e-6``、AMDで ``2.28882e-5`` だった。
  非ゼロ原点、PAR/mipmapを反映したcanonical境界、ROI、パン・ズーム、
  bilinear、LUT/gamma/gain、負値/HDR、alpha、全9比較モード、
  A/Bの欠落と同じ入力の差分比較を含む。
  上から下へ格納されたpremultipliedラスタ画像の合成と、更新対象外を保つ
  partial更新も含む。未初期化ターゲットへの合成は拒否する。
* ネイティブウィンドウの実フレームで、A/Bワイプ、パン・ズーム、sRGB表示、
  リサイズ後の再描画を確認した。両GPUのDPR 1.0 / 1.5でCPU期待値と一致した。
* リソース解放まで含めてValidation/Synchronization Validationのエラー数0。
  Renderer単体は両GPUともOpenGL DLLを読み込まなかった。
* 転送境界の変更後もNatron本体のビルド、既存CTest、GuiSmoke、OfxGuiSmokeが
  通った。GUI smokeの予期しない確認ダイアログは0件だった。

比較基準に使った旧Viewerには、テクスチャのbit depth変更時に新しいテクスチャを
作っても古いテクスチャへ転送し続ける不具合があった。
32fのGuiSmokeで黒画像を再現し、転送先の参照を更新して修正した。
修正後は正常な画像が表示された。このGL転送処理は本体Vulkan移植で除去した。
``-GuiTextureDepth 8u`` / ``32f`` で
テスト用プロファイルの表示深度を指定できる。ユーザー設定は変更しない。

表示のNaN/Infと変換・比較合成中のoverflowはマゼンタにする。元画像は変更しない。
非単位gamma/BT.1886の負値は0へ制限してからpowを評価する。
旧GL実装の未定義なpow結果を再現するものではない。
上記はCPUモデルと実フレームによる検証であり、既存NatronのGLフレームとの
網羅的な比較はまだ残っている。HDR比較時の中間clamp/量子化、画像端の拡張、
partial overlay、色拾い、OCIO、Roto/Trackerの統合も完了条件に含める。

RendererのJSONは各GPUの ``Renderer.stdout.log`` に保存する。
表示のPNGは ``viewport-wipe.png``、``viewport-pan-zoom.png``、
``viewport-srgb.png``、``viewport-srgb-resized.png`` として保存する。

Natron Viewer integration
-------------------------

``Gui/ViewerGL`` はQWidgetとなり、子のネイティブ ``VulkanCanvas`` へ表示する。
クラス名と ``OverlaySupport`` の旧API名は後続の移植が完了するまで残るが、
Viewerの画像転送・表示・文字・ROI・ワイプハンドル・picker表示にGL呼び出しはない。
PBO、GLテクスチャ、GLSL表示シェーダー、VAOとGL描画用コードを除去した。
``Gui/ViewerVulkan.cpp`` が表示処理を受け持つ。

複数Viewerは同じインスタンスと論理デバイスを共有する。画像はRGBA8/RGBA32Fで
保持し、canonical座標とPAR/mipmapを引き継いで表示する。通常の表示にreadbackはなく、
テストや明示的なキャプチャ時だけスワップチェーンから取得する。
ネイティブウィンドウの入力をViewerへ転送し、既存のズーム・パン・ROI・色拾い処理を
使う。色拾いの計算は元のRAM画像を使う。

ホストのformat/RoD、ROI、wipe、selection、picker、persistent messageと文字は
QPainterで透明ラスタ画像に描き、Vulkanで元フレームに合成する。
ラスタの行向きとpremultiplied alphaを明示した。
フォントサイズと入力座標は論理ピクセルで扱い、物理ピクセルへの変換はDPRで行う。
Viewerのfloat表示設定は旧GL拡張の有無に依存しない。

Roto/RotoPaintの輪郭・制御点・羽根・選択枠・ブラシと、ホスト標準の位置・変形・
Corner Pinハンドルを ``Engine/OverlayDrawList`` へ移した。このリストは正規座標、
頂点ごとのRGBA、論理ピクセルの線幅、点サイズ、stipple、3x3変換、画面方向の影、
UTF-8文字を保持し、GuiやGLへの依存を持たない。``Gui/OverlayRasterizer`` が透明画像へ
ラスタライズし、既存のVulkan合成経路へ渡す。ノードごとに状態と変換を復元する。
Rotoの描画と編集入力を再び有効にし、Trackerの標準変形・Corner Pinハンドルも使える。
ホスト標準ハンドルは対応するプロパティページが有効な間だけ表示する。

Trackerの追跡マーカー・軌跡・拡大画像・キーフレームのサムネイルも共通命令へ移した。
画像はCPU上の上から下へ並ぶRGBA8として保持し、描画命令が参照を所有する。
非同期レンダーの完了時にGLコンテキストやPBOを要求せず、画像の作成・破棄も
GL関数を呼ばない。拡大時のUVと端のクランプ、ラベル・ハンドルの重ね順を保持する。
拡大時はnearest、縮小時はlinearとし、拡大画像の画素境界を保つ。

OFX 1.5 DrawSuite V1 / Overlay Interact V2
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``Engine/OfxDrawSuite`` に標準C ABIの6つの描画関数を実装した。
公式ヘッダーはOpenFXの ``adb790bf0043714cae6091d58791e8cdabdd9e12`` から
``Global/OpenFX`` に改変せず取り込み、出典とBSD-3-Clauseライセンスを保持する。
既存のHostSupportサブモジュールを変更せず、ホストのsuite取得とdescriptorに
V2エントリーポイントを追加した。V1/V2を両方持つプラグインはV2を選ぶ。
ホストのOFX APIバージョンは1.5を返す。

描画中だけ有効なハンドルをスレッド単位で管理し、未知のポインターを参照しない。
描画終了後・別スレッドからの呼び出しは ``kOfxStatFailed`` を返す。
入れ子の描画でも各ハンドルを分離し、描画状態を復元する。
図形、破線、UTF-8文字と配置指定は共通命令へ変換し、半透明色はover合成する。
幅0は1物理ピクセル、非0の線幅は表示密度に合わせる。
無効な引数とC++例外はOFXステータスへ変換し、不完全な描画命令を取り消す。

``Tests/DrawSuitePlugin.cpp`` はNatron/Qt/描画APIへリンクしない独立したOFXプラグインである。
V1に失敗するエントリーを設定してV2選択を検査し、実際のViewerへ描画して
パラメーターをドラッグで変更する。``tools/ofx-drawsuite-gui-smoke.py`` が実フレームの
半透明RGBA、1物理ピクセル線、操作後の再描画、描画範囲外のハンドル拒否を確認する。
このテストのときだけ ``build/ofx-tests`` をプラグイン検索へ加える。
コアの寿命・スレッド・入れ子・不正入力・変換・文字配置もC++テストで検査する。

ネイティブウィンドウとcontainerのフォーカス通知をViewerへ明示的に転送し、
同じ状態のWidget通知は重複させない。プラグインがモーダルダイアログを開いている間は
通知を保留し、終了後の描画・入力前にlost/gainedの順で届ける。
OFXテストプラグインがMessageSuiteから開くダイアログを自動で閉じ、
モーダル中の再入呼び出しがなく、次の入力前にドラッグが解除されることを検査する。
テストが意図して開くダイアログは内容で識別し、それ以外のモーダルは失敗として報告する。

TrackerのCtrlによるoffset操作、キー解放なしのフォーカス喪失、Ctrl+Altによる
一時的なトラック追加、手動の追加モード保持、ドラッグ中断も実サーフェスで検査する。
フォーカスを失うと修飾キーと一時的な追加モードを解除し、進行中のドラッグを取り消す。
手動で選んだ追加モードは修飾キーの解放やフォーカス喪失でも保持する。

**開発中の機能制限:** 配布OFXの旧GL編集オーバーレイは引き続き移植対象である。
``ViewerTab`` と ``EffectInstance`` は描画と入力を同じ
対応条件で選び、未移行のプラグインのGL関数をVulkanのウィンドウから呼ばない。
DrawSuite/V2を持つプラグインは描画と入力を有効にする。カーブ、Dope Sheetなどの
GL Widgetも残るため、完全移行の完了とは扱わない。

本体のGUI検証はVulkan backend、実際のフレーム、検証エラー、共有デバイスIDを
確認する。ネイティブサーフェスへ中ボタンドラッグとホイールを送り、canonical位置と
ズームの変化・復元を確認する。OFXの4系統をつないだGUIグラフも同じViewerへ表示する。
終了後のstderrも調べるため、ウィンドウ解放時のVulkanエラーは成功扱いにならない。

2026-10-11にGPU 0/1、8u/32f、GuiSmoke/OfxGuiSmokeの8ケースが通り、
表示中と終了時の検証エラー、予期しないモーダルは0件だった。
追加の4色画像は両GPU・両深度で四隅のRGB値が一致し、キャッシュ転送の行向きと
チャンネル順を確認した。結果は ``build/vulkan-natron-quadrant-tests`` に保存した。
両GPUの32f・DPR 1.5も通り、``build/vulkan-natron-quadrants-dpi150`` に保存した。
追加検証ではNativeFormatのユーザー設定を実行前後に読み取り、値のfingerprintが
同一であることを確認した。CPUのCTestも専用プロファイルで通った。

Rotoの輪郭・制御点、ホスト標準のTransformとCorner Pinを共通描画命令へ移した後、
GuiSmoke/OfxGuiSmoke/OverlaySmokeの12ケースが両GPU・両深度で通った。
150%表示倍率でもOverlaySmokeの4ケースが通り、ネイティブサーフェスへのドラッグで
制御点・変形・Corner Pinの値が更新された。表示・終了時のVulkan検証エラーと
予期しないモーダルは0件で、ユーザー設定のfingerprintも一致した。
結果は ``build/vulkan-natron-native-overlays`` と
``build/vulkan-natron-native-overlays-dpi150`` に保存した。
描画命令の変形・影・頂点色・不正座標を調べる3件のCPUテストも通った。

Tracker移行後も12ケースが通った（``build/vulkan-natron-tracker-final``）。
拡大画像とキーフレームのサムネイルには読込をsRGB、出力をLinearに指定した4色画像を
使い、中間色のRGB、上下の向き、チャンネル順を確認した。マーカーのドラッグと
拡大窓のリサイズ・ドラッグで、対応するパラメータが更新されることも確認した。
nearestによる画素境界を確認する追加の4ケースと150%表示倍率の4ケースも通った。
結果は ``build/vulkan-natron-tracker-nearest`` と
``build/vulkan-natron-tracker-dpi150`` に保存した。各レポートのVulkan検証エラーと
予期しないモーダルは0件で、ユーザー設定のfingerprintも一致した。
描画命令による画像参照の保持と不正画像の検査を加え、CPUのCTestも通った。

DrawSuite/V2追加後、従来のGUI/OFX/ネイティブ操作と新しいDrawSuiteプラグインを含む
16ケースが通った（``build/vulkan-natron-drawsuite-final``）。DrawSuiteの両GPU・両深度は
DPR 1.5でも通った（``build/vulkan-natron-drawsuite-dpi150``）。半透明図形のRGB値、
幅0の1物理ピクセル線、マウスによるパラメーター変更と再描画を確認した。
V1エントリーと不正なコンテキストの受理は0件で、期限切れコンテキストは拒否された。
両マトリクスでユーザー設定が一致し、Vulkan検証エラー・予期しないモーダルは0件だった。
テストプラグインのPE importはCRTとKERNEL32だけで、Qt/描画APIへの直接依存はない。

フォーカス転送とモーダル復帰処理を追加した後も16ケースが通った
（``build/vulkan-natron-focus-final``）。DPR 1.5ではOverlaySmoke/DrawSuiteSmokeの
8ケースが通った（``build/vulkan-natron-focus-dpi150``）。OFXのモーダル中の
フォーカス再入は0件で、終了後の入力前にドラッグ状態が解除された。
Trackerの一時的な追加モードの解除、手動モードの保持、修飾キーとドラッグ中断も通った。
Vulkan検証エラー・予期しないモーダルは0件で、ユーザー設定のfingerprintは一致した。
専用プロファイルのCTestも通った（``build/native-focus-core-tests.log``）。

タイムラインのOpenGL廃止
----------------------------------------------------------------

``TimeLineGui`` は ``QOpenGLWidget`` から ``QWidget`` に移した。
OpenGLコンテキスト、直接のGL呼び出し、GL用の文字描画を除去し、
既存の ``OverlayDrawList`` と ``QPainter`` で目盛り・マーカー・文字を描画する。
座標変換は連続座標を保ち、フォントと線幅の表示倍率はQtに任せる。
Viewerの画像とオーバーレイの合成・表示は引き続きVulkanが担当する。

``TimelineSmoke`` はキャッシュとキーフレームの実画像の色、フレーム移動、
再生範囲編集、ホイールズーム、パン、右ボタンの範囲選択、タイムコード文字を確認する。
再生範囲編集はViewerの再生範囲だけを変え、プロジェクト全体の範囲は維持する。
共通の描画器は任意の座標変換関数も受け取り、Viewer用の既存の入口を維持する。

タイムライン移植後の標準倍率の20ケースが通った
（``build/vulkan-natron-timeline-final``）。Overlay/DrawSuite/Timelineの両GPU・両深度を
DPR 1.5でも確認し、12ケースが通った（``build/vulkan-natron-timeline-dpi150``）。
キーフレームの色は1フレーム幅のバー中央で測定し、端の部分的なピクセル被覆を避ける。
フォントの論理サイズは両倍率で同じだった。32ケースのVulkan検証エラーと
予期しないモーダルは0件で、ユーザー設定のfingerprintは一致した。
JSONには検証した本体のSHA-256も記録した。
専用プロファイルのCTestも通った（``build/timeline-core-tests.log``）。

ヒストグラムのOpenGL廃止
----------------------------------------------------------------

``Histogram`` も ``QWidget`` と共通の ``OverlayDrawList`` / ``QPainter`` に移した。
現在使っているCPU集計を維持し、未使用のGL集計・シェーダー・FBO・テクスチャの
条件付き実装を除去した。目盛り・文字・ピッカーは通常のQt描画を使う。
RGBの棒は加算合成する。Qt 6.11.2で隣接棒のアンチエイリアスと加算の色ずれを
独立したQImageと実パネルで再現したため、棒は従来同様の平滑化しない線で描画する。
他のオーバーレイの既定の平滑化は維持する。

CPUの結果取得は最新のスナップショットを返して古い結果を破棄し、残る通知は安全に処理する。
現在の表示モード・幅・範囲と合わない結果は受け取らない。
範囲外や空のピッカーは0を返し、Viewerピッカーのアルファ線も描画コマンドに含める。
``HistogramSmoke`` は棒の実画素、RGB/A/Y/R/G/B、連続したモード変更、平滑化、
名前指定と現在のViewer、入力A/B、RGB/アルファピッカー、画像変更、範囲外ピッカー、
パン・ズーム・Fキーの表示リセットを確認する。

移植後の標準倍率の24ケースが通った（``build/vulkan-natron-histogram-final``）。
HistogramのGPU 0/1・8u/32fをDPR 1.5でも確認し、4ケースが通った
（``build/vulkan-natron-histogram-dpi150``）。両倍率でRGBの重なりは白になり、
各チャンネルの棒・Viewerのアルファ線・範囲変更・入力復帰も通った。
28ケースのVulkan検証エラーと予期しないモーダルは0件で、
ユーザー設定のfingerprintとフォントの論理サイズは一致した。
ケース別JSON・PNGと本体のSHA-256を保存した。
専用プロファイルのCTestも通った（``build/histogram-core-tests.log``）。

OFXカスタムパラメータのDrawSuite移植
----------------------------------------

``CustomParamInteract`` を ``QWidget`` と ``OverlayDrawList`` / ``QPainter`` に移した。
GL Widget、GL context、GL描画・属性保存は使わない。座標とサイズは論理ピクセルとし、
左下を原点にする。半透明描画、線幅0の物理1px線、文字とリサイズに対応する。

OpenFX 1.5.1の標準 ``kOfxParamPropInteractV1`` はGL専用であり、
標準のカスタムパラメータ用DrawSuiteエントリポイントはまだない。
``Global/OpenFX/ParamInteractDrawSuite.h`` にNatron独自の拡張を定義した。
ホストの ``kNatronOfxParamHostPropSupportsDrawSuiteInteract`` が1なら、
プラグインは値パラメータの ``kNatronOfxParamPropInteractDrawSuiteV1`` に
完全なカスタムコントロールの ``OfxPluginEntryPoint*`` を設定できる。
通常のOFX interact actionを使うが、描画はDrawSuite V1で行う。
``kOfxInteractPropDrawContext`` はDrawの間だけ有効であり、入力には渡さない。
ホストが背景を消去し、バッファ交換とGL contextは提供しない。

標準 ``kOfxParamHostPropSupportsCustomInteract`` は0にし、GL専用の完全な
カスタムコントロールを廃止した。拡張を指定しないパラメータは通常のコントロールを使う。
パラメトリックカーブの背景描画は別のAPIであり、以下のカーブ移植で拡張した。

フォーカス通知の重複をまとめ、プラグインのモーダルaction中は描画・入力・フォーカスを
再入させない。モーダル終了後に保留したフォーカス喪失を通知し、プラグインのドラッグと
修飾キー状態を解除する。キーのUTF-8文字列はDown/Repeat/Upに渡す。

独立したC ABIプラグイン ``ParamDrawSuiteSmoke`` はQtとグラフィックスAPIにリンクしない。
失敗するV1と拡張を同時に宣言し、拡張だけが呼ばれることを検査する。
V1だけを指定した別パラメータにはカスタムWidgetを作らず、通常のUI経路を使う。
実画素の半透明合成、線幅0、期限切れDraw context、論理サイズとDPR、
小数座標の編集、UTF-8キーとRepeat、リサイズ、モーダル中の通知抑止をGUIで検査する。
意図したOFX確認ダイアログだけを識別して閉じ、予期しない確認は失敗として報告する。
テスト専用の設定・自動保存領域、終了方法とPID監視を使い、通常設定は変更しない。

移植後の標準倍率28ケース（``build/vulkan-natron-param-final``）と、
カスタムパラメータのGPU 0/1・8u/32fをDPR 1.5で実行した4ケース
（``build/vulkan-natron-param-dpi150``）が通った。倍率1.5では細線の測定範囲に
隣の破線が入ったため範囲を修正し、標準倍率の同じ4ケースも再検証した
（``build/vulkan-natron-param-standard-recheck``）。最終の28ケース集計はこの再検証結果を参照する。
CTestも通った（``build/param-core-tests.log``）。32ケースのケース別JSON・PNG、
本体SHA-256とユーザー設定fingerprintを照合し、Vulkan検証エラーと予期しない
確認ダイアログは0、ユーザー設定の変更もなかった。
本体SHA-256は ``16e6cdfab44001bad1896cc7b1757e4926d9fb6ab3dd731a1508527b6028da91``。
この検証はカーブ移植前のバイナリに対する結果である。

カーブとOFXパラメトリック背景の移植
----------------------------------------

``CurveWidget`` / ``CurveGui`` を ``QWidget`` と ``OverlayDrawList`` /
``QPainter`` に移した。GL Widget、GL属性、TextRendererを使わない。
スケールと座標は論理ピクセルとし、キー、接線、選択枠、グリッド、時間マーカーと
ラベルを同じ描画リストで描く。Curveの編集・Undoとシリアライズは保持する。

標準のパラメトリック背景コールバックはGL専用なので呼ばない。
``kNatronOfxParamHostPropSupportsDrawSuiteParametricBackground`` が1なら、
``kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1`` に背景エントリを
設定できる。Describe/Create/Destroy/Drawを受け取り、編集入力はホストが処理する。
背景の座標はカーブのcanonical座標で、Draw contextはDraw中だけ有効である。

``NatronDrawSuite.h`` は任意の追加suiteであり、標準DrawSuite V1と同じcontextを使う。
平行四辺形の2辺間のグラデーション、SourceOver/Add、アンチエイリアスを提供する。
背景の隣接勾配・ヒストグラムは旧描画と同じく平滑化しない。状態はDraw終了時に戻し、
ホストのカーブに加算合成を漏らさない。skew付きの勾配は辺の法線方向に補間する。
これはNatron独自拡張であり、標準OpenFX APIの拡張とは扱わない。

配布MiscのColorLookup、HueCorrect 1/2、HueKeyerの背景をこの経路へ移した。
グラデーション、ヒストグラムの加算合成、Hue Vs Hueとピッカーの元の形状を保持した。
再現可能なパッチと補助ヘッダを ``build-configs/windows-ofx`` に保存し、
WindowsのOFXビルドで適用する。これらの背景ソースにはGL呼び出しがないが、
Miscモジュールの他ノードにはGL経路が残る。

独立したC ABIプラグイン ``CurveDrawSuiteSmoke`` はQt/描画APIにリンクしない。
``CurveSmoke`` はnative背景の選択、期限切れcontext、グレー/斜め勾配と加算合成の
画素、キー移動、Undo/Redo、接線編集、ズーム・パンを検査する。配布4背景の
参照画素、ColorLookupヒストグラム、Hue Vs Hueと通常の時間カーブのscrubも検査する。

移植後の標準倍率32ケース（``build/vulkan-natron-curve-final``）と、
CurveSmoke/ParamDrawSuiteSmokeのGPU 0/1・8u/32fをDPR 1.5で実行した8ケース
（``build/vulkan-natron-curve-dpi150``）が通った。CTestも通った
（``build/curve-foundation-core.log``）。全40ケースのケース別JSON・PNGと
本体SHA-256を照合し、Vulkan検証エラー、予期しない確認ダイアログは0だった。
フォントの論理サイズは両倍率で11ptを保ち、PNG寸法は論理サイズ×DPRと一致した。
ユーザー設定fingerprintもテスト前後で一致した。照合結果は
``build/curve-validation-proof.json`` に保存した。
本体SHA-256は ``ac4fe588d1315441c0af7912bde9893339de35d7c1ff2969bd8a1a1d83371a0a``。
Engineと残るOFXのGL経路は引き続き移植対象である。

Dope SheetのOpenGL廃止
------------------------

``DopeSheetView`` を ``QOpenGLWidget`` から ``QWidget`` と
``OverlayDrawList`` / ``QPainter`` に移した。GLコンテキスト、GLテクスチャと
直接のGL呼び出しを除去し、元の18種類のキーアイコンを論理ピクセルで描く。
スケール、選択枠、時間マーカー、Reader/Time/Groupの範囲も同じ描画リストを使う。
EngineにGuiへの依存は追加せず、シリアライズとPython APIは保持した。

キー通知では多成分パラメータのrootを更新し、非表示・折り畳み行のキーを描かない。
成分を選択した貼り付けが自動選択されたrootを通じて未選択成分にも書く問題と、
選択枠の拡大縮小が古いマウス位置を使う問題を修正した。
フォーカス喪失、ウィンドウの無効化、非表示とmouse grab喪失では編集状態を解除し、
確認ダイアログでドラッグが中断されても状態を残さない。
全体表示には余白を設け、範囲の端にあるキーアイコンも欠けないようにした。
カーブの時間マーカーは小数を含むマウス座標から最も近いフレームを選び、
整数ピクセル化と切り捨てによる1フレームのずれも修正した。

``DopeSheetSmoke`` はキーの選択、移動、拡大縮小、全補間アイコン、
コピー・貼り付け・削除、Undo/Redo、scrub、時間方向のzoom/panを検査する。
Readerの左右trim・slip・移動、TimeOffset/FrameRange/Groupの移動とUndo、
Retimeを含む範囲の表示、折り畳み・非表示・scroll、resizeと
テスト所有のモーダルダイアログによる編集中断も検査する。

移植後の通常倍率36ケース（``build/vulkan-natron-dope-final``）と、
DopeSheetSmoke/CurveSmoke/ParamDrawSuiteSmokeのGPU 0/1・8u/32fを
DPR 1.5で実行した12ケース（``build/vulkan-natron-dope-dpi150``）が通った。
CTestも通った（``build/dope-foundation-core.log``）。全48ケースのケース別JSON・PNG、
実行ファイル、テスト定義とチェックポイントのSHA-256を照合した。
Vulkan検証エラー・予期しない確認ダイアログは0件で、ユーザー設定fingerprintは
テスト前後で一致した。フォントは両倍率で11ptを保ち、PNG寸法は
論理サイズ×DPRと一致した。照合結果は ``build/dope-validation-proof.json`` に保存した。
本体SHA-256は ``d776eea6ea0301b69bf15ea74fab9a6c7000eaf99bd75ba9da6389bd903579d4``。

長時間テストは完了したケースごとに実行ファイル・テスト定義・設定・JSON・PNG・ログの
ハッシュを保存する。``-Resume`` はそれらとGPU・bit depth・表示倍率が一致する場合だけ
再利用する。中断時の ``report.partial.json`` は成功を示さず、全ケース完了と
設定・実行ファイルの前後照合を終えた場合だけ成功の ``report.json`` を保存する。
集計とチェックポイントの書き換えは一時ファイルからの置換とする。
Engineと配布OFXの残るGL実行経路・依存除去は引き続き移植対象であり、
本体全体がOpenGLなしで動作することはまだ検証できていない。

.. code-block:: powershell

   .\tools\test-windows-vulkan-natron.ps1

GPU 0/1、8u/32f、GuiSmoke/OfxGuiSmoke/OverlaySmoke/DrawSuiteSmoke/
ParamDrawSuiteSmoke/CurveSmoke/DopeSheetSmoke/TimelineSmoke/HistogramSmokeの36ケースを実行する。
``build/vulkan-natron-tests`` にケース別JSON、表示PNG、ログを保存する。
専用の設定・自動保存領域とPID監視は引き続き使う。
``NATRON_VULKAN_DEVICE`` は開発テスト用の物理デバイス選択であり、未指定なら
対応するディスクリートGPUを優先する。

GUIのQt OpenGL依存廃止
------------------------

GUIの最後の直接GL描画だったDope Sheet移植後、呼び出し元のない
``Gui/TextRenderer`` と ``Gui/Shaders`` を削除した。CMakeのQt必須component、
Guiのinclude pathとリンクからOpenGLWidgetsを外した。
ビルドにQt OpenGL / OpenGLWidgets componentを要求しない。

Pythonの標準初期化はPySide6の ``QtCore`` / ``QtGui`` / ``QtWidgets`` に統一した。
QtPyの互換初期化が使わないQtDataVisualizationやQtOpenGLを読み込んでいたためである。
明示的にQtPyを使うユーザースクリプト用には依存パッケージとbinding選択を保持した。
標準起動のGUIテストはWindowsのロード済みモジュールも検査し、
Qt6OpenGL.dll / Qt6OpenGLWidgets.dllが存在しないことを必須とする。
EngineのWGLや配布OFXによるsystem OpenGL依存はまだ残っており、
この変更だけでアプリケーション全体のGL実行経路を廃止したとは判定しない。

150%表示の再検証では、Readerのslipハンドルと次の行のクリップ本体が
重なる位置で誤ったノードを動かす問題を確認した。ノードのポインタ順で
判定していたためである。Readerの端のハンドルを本体より先に判定し、
最も近いハンドルを選ぶ。hoverのカーソルとクリックは同じ判定を使う。
回帰検証では両クリップの時間範囲が重なる位置でslipし、Readerのパラメータ、
範囲、Undoと隣のTimeOffsetの値を検査する。

GUI起動の検査は、45秒を上限として4色の検証画像の到着を待つ。
最初の背景フレームと画像の表示完了を区別し、待機中もモーダルダイアログと
描画エラーを検査する。PySideで取得したウィジェットとネイティブウィンドウの
参照はテスト終了まで保持する。ウィンドウの表示状態と待機時間も結果に保存する。

最終実行ファイルの通常倍率36ケース
（``build/vulkan-natron-gui-no-qtgl-reader-final``）、起動検査を更新した通常倍率4ケース
（``build/vulkan-natron-gui-no-qtgl-reader-gui-ready``）、150%表示16ケース
（``build/vulkan-natron-gui-no-qtgl-reader-dpi150``）が通った。GPU 0/1と8u/32fを含む。
CTestとヘッドレスのUnicode画像入出力・プロジェクト保存/再読込も通った。
通常倍率36ケースのテスト定義は退避した元の起動スクリプトとSHA-256で照合し、
起動検査以外のスクリプトが同一であることも検査した。
各集計はそれぞれのテスト定義、ケース別JSON・PNG・ログ、チェックポイントと
実行ファイルのハッシュを照合した。検証結果は
``build/gui-no-qtgl-validation-proof.json`` に保存した。
本体SHA-256は ``54172a71c4bc0d7b94e1948446c45462000f23b31588ca0670a82e63d2a6fca2``。

全56ケースでVulkan検証エラーと予期しないダイアログは0件、ユーザー設定の
fingerprintは一致し、編集用フォントは11ptを保った。
GUI起動の両倍率でQt6OpenGL.dll / Qt6OpenGLWidgets.dllは読み込まれていない。
同じQt SDKのQt Widgets単独起動でも、これらとsystem OpenGLは読み込まれなかった。
配布OFXの再監査は5モジュール・579バージョンを確認し、Arena / IO / Miscの
3モジュールにGLの直接依存が残っていた。監査結果は
``build/ofx-vulkan-audit-gui-no-qtgl`` に保存した。

EngineのネイティブVulkan画像処理
--------------------------------

``Gpu/ImageOperations`` にRGBA32FのROI付きgain/offset、チャンネル復元、
マスク/mix合成を追加した。コピーはRGBA8/16 UNORM、RGBA16/32 floatに対応し、
重なる自己コピーは一時画像を経由する。入力と出力を共有するComputeも一時画像を
経由し、ROI外は保持する。未初期化の出力のROI外はゼロで初期化する。

``Engine/OfxVulkanImageSuite`` はGUIに依存しないC ABIでこの処理へ接続する。
公開ヘッダは ``Global/OpenFX/NatronVulkanImageSuite.h``。
標準OFXのCPU clip画像から明示的にupload/downloadし、Vulkan画像はcontextに
属するopaque handleで管理する。同じcontextの呼出しは直列化し、解放済みhandleや
別contextの画像は拒否する。行の向きは変更せず、負のrowBytesとパディングを扱う。

このsuiteはNatron固有のversion 1拡張である。標準OFXのGL texture handleを
Vulkan画像に置き換えるものではない。GUI Viewerとのlogical device共有、
Engineの通常レンダー経路への接続、配布OFXのGPU処理の移植は残っている。
この実装だけでは旧GL画像処理は廃止されない。

検証には ``Tests/VulkanImageSuitePlugin.cpp`` の独立したOFXプラグインを使う。
Natron/Qt/Vulkan/OpenGLライブラリをリンクせず、hostからfetchしたsuiteで処理する。
実レンダーの全RGBA画素をCPU参照と比較し、さらにC ABIのhandle、signed row stride、
同時呼出し、project保存・読込を検査する。
``tools/test-windows-vulkan-engine.ps1`` は専用profile、PID限定watchdog、
設定fingerprint、実行ファイル・ソース・結果のSHA-256を記録する。

2026-10-11の検証ではRTX 5080とAMD Radeon(TM) Graphicsのそれぞれで
660ケースのCPU参照比較、10ケースの入力拒否、4 host workerの同時呼出しが成功した。
独立したOFXプラグインの実レンダーは各GPUで2回以上実行され、全画素比較の
最大誤差は ``5.960464477539063e-08``、native Compute全体では
``4.76837e-07``。両GPUでVulkan検証エラーは0件である。
標準OFX clipの変化が出力へ届くこと、project保存・再読込も確認した。
結果は ``build/engine-vulkan-tests/report.json`` に保存した。

通常のRenderer自動レンダーが検証後にWriteノードを再実行し、画像を上書きする
問題を避けるため、このテストは ``--interpreter`` で実行する。
このモードでEOF時の終了クラッシュも見つかった。既に初期化された組込みPythonで
``Py_Main`` を呼ぶ処理を ``PyRun_InteractiveLoopFlags`` に変更し、runtimeの
終了処理をAppManagerが管理するようにした。GILの取得・解放も対応させた。
テスト後にstdinの追加コマンドを実行してEOFで正常終了することと、検証した
画像・projectのSHA-256がプロセス終了後も一致することを確認した。

同じ最終ビルドでCTest、既存OFXレンダーとproject roundtrip、GUIの
2 GPU × 2 texture depthの4構成が成功した。GUIのQt OpenGL DLL、
予期しないダイアログ、Vulkan検証エラーは0件で、ユーザー設定のfingerprintは一致した。
GUI結果は ``build/vulkan-natron-engine-image-ops/report.json``。
Natron本体SHA-256は
``1502e89f502bed430854647641ca9dea7533f6fc2cedb8952a05f8db9f2ff82d``。
NatronRendererは
``789889712e1223796b630d967aa97095eab297accf91f752eb680a88c97c2588``。

Engine ImageとキャッシュのVulkan保存
------------------------------------

``eStorageModeVulkan`` を追加し、``Image`` / ``CacheEntry`` の保存先として
``VulkanImageStorage`` を接続した。既存のstorage modeの値は維持する。
contextは実行時だけ保持し、projectやdisk cacheへシリアライズしない。
現在の形式はFloat RGBAで、画像の確保時には全画素をゼロで初期化する。
OFX Vulkan suiteの画像もこのEngine画像を所有するよう変更した。

RAMとの転送、異なる原点を持つ画像間のROIコピー、fill、mask/mix、
チャンネル復元、画像範囲の拡張を既存のImage操作から呼び出せる。
ROI外は保持し、拡張した領域はゼロになる。描画済みbitmapと画像のVRAM相当サイズを
キャッシュの確保・サイズ変更・解放通知に反映する。画像が互いをコピーする場合も
同じ順序でlockを取得し、入力と出力の共有はComputeの一時画像で処理する。
同じlogical device上でも、別contextの画像同士の操作は拒否する。

fillは入力を読まない専用処理を使い、NaNを含む領域もゼロにできる。
CPU版mask/mixでoriginalがない場合に処理を省略していた不具合も修正した。
欠けたoriginalをゼロとして合成し、CPUとVulkanの結果を合わせる。
GPUを必要としないCTestにも、このケースの回帰検査を追加した。

2026-10-11の最終ビルドで、RTX 5080とAMD Radeon(TM) Graphicsそれぞれの
Engine画像95ケースとnative Compute660ケースが成功した。Engine画像では
64回の双方向同時コピー、キャッシュ解放と範囲拡張、bitmapの保存、contextの分離を
確認し、CPU参照との最大誤差は ``1.19209e-07`` だった。
独立したOFXプラグインの実レンダーは各GPUで2回成功した。
CTest、配布OFXのレンダーとproject roundtrip、GUI / Overlayの
2 GPU × 2 texture depth × 2種類の8構成も成功した。
Vulkan検証エラーと予期しないダイアログは0件、ユーザー設定のfingerprintは一致した。

記録は ``build/engine-vulkan-storage-tests/report.json``、
``build/vulkan-natron-image-storage/report.json``、実行ファイル・ソース・
出力artifactを照合した ``build/vulkan-image-storage-validation-proof.json``。
Natron本体SHA-256は
``9a31b19f695260e0a559a52a9224a80d5aff0ee39a0055b48eb49511b1db3a04``。
NatronRendererは
``5a26a7527f2c22057c0c5f6e0d29693e3bf249a4653dfab8305ab8e7510cd827``。

この保存先は明示的なVulkan画像とOFX suiteに接続済みだが、
``EffectInstance::renderRoI`` の通常GPU選択、mipmapと形式変換、Viewerとの
logical device共有にはまだ接続していない。EngineのGL処理と配布OFXのGPU shaderは
引き続き移植対象である。

ランタイムfragment shaderとShadertoy
------------------------------------

``Gpu/FragmentProgram`` を追加し、shadercを静的リンクしてGLSLを実行時に
SPIR-Vへコンパイルする。Vulkanのdynamic renderingでFloat RGBAへ描画し、
微分命令を含むfragment shaderを実行する。4個のsampler2Dとstd140のuniformを
受け取り、nearest / linear / mipmap / anisotropicとwrapを処理する。
ROI外の画素は保持し、入力と出力が同じ画像の場合は一時画像からROIをコピーする。
``gl_FragCoord`` のxyは画像座標、z / wは従来と同じ0.5 / 1になる。

``NatronVulkanShaderSuite.h`` のV1は、既存のVulkan Image Suiteのcontextと
Engine画像を共有する。programの作成・解放・描画と使用中resourceの取得を提供し、
期限切れhandle、別context、uniformサイズとコンパイル失敗をC ABIで返す。
公開する画像とprogramはopaque handleで、GLやVulkanの内部handleを渡さない。

配布MiscのShadertoyにこのsuiteを利用する経路を接続した。
NatronではGL contextを初期化せず、既存のnode ID、projectとパラメーターを使って
Vulkanへ描画する。旧texture関数、precision宣言と組み込みuniformを変換し、
bool / int / float / vec2 / vec3 / vec4のユーザーパラメーターを転送する。
Auto Paramsは最適化後SPIR-Vの使用中resourceから取得し、未使用uniformや入力を
有効にしない。Renderer Infoには実行したVulkan deviceとAPIを表示する。
コンパイル失敗はプラグインのエラーとして返し、確認ダイアログを開かない。
Windowsの再ビルドは統合した ``openfx-misc.patch`` と
``native/ShadertoyVulkan.cpp`` から再現できる。

2026-10-11の最終ビルドで、RTX 5080とAMD Radeon(TM) Graphicsそれぞれの
fragment shader40ケース、不正入力7ケース、4 workerの並行処理が成功した。
CPU参照との最大誤差は ``6.3777e-06``、nativeテストでOpenGLのロードは0件。
配布Shadertoyの実レンダーは各GPUで19ケース、236873画素を比較した。
組み込みuniformと微分、6種のパラメーター、Auto Params、4入力、filter / wrap、
配布Gaussian Blur preset、日本語名projectの保存・再読込を含む。
Rendererのstdin EOFと終了後の出力artifactのSHA-256も確認した。

Engine画像95ケースとCompute660ケース、独立OFXの実レンダー2回を各GPUで
再検証した。CTest、通常の配布OFXレンダーとproject roundtrip、
GUI / Overlayの2 GPU × 2 texture depth × 2種類の8構成も成功した。
検証したVulkan経路のvalidation errorと予期しないダイアログは0件。
ユーザー設定のfingerprintも実行前後で一致した。

記録は ``build/shadertoy-vulkan-tests/report.json``、
``build/engine-vulkan-fragment-tests/report.json``、
``build/vulkan-natron-shadertoy/report.json`` と、ソース・実行ファイル・出力を
照合した ``build/vulkan-shadertoy-validation-proof.json``。
Natron本体SHA-256は
``df163a4c53f49e1d0638ff9b9760f9efa7071c50357210240c4f0654055b8818``。
NatronRendererは
``745d600a3c32931dfa7c3c4461903bea2aaedb77968e807a5a8464b0a86f235f``。
配布Miscは
``d5f184b4ebb7e9d021ad6578fe71d6e2ecc332f7062eb7e643dbc4508dd817a2``。

Shadertoyのpreset全件はまだ検証していない。標準OFX clipは引き続きCPU画像で、
この経路ではVulkanへ明示的に転送する。mipmapは描画ごとに作成する。
他host用のGL fallback、Engineの通常GPU選択、
Viewerとのlogical device共有と追加形式の画像処理は移植対象として残る。

Shader Suite V2と配布OCIOのVulkan化
--------------------------------------------------

``NatronVulkanShaderSuiteV2`` はV1を保持し、入力とuniformのbinding指定、
不変の1D / 2D / 3D LUTを追加した。LUTはfloatの1 / 3 / 4成分を受け取り、
生成時に内容を所有する。descriptorの重複、shaderとのbinding種類・画像次元の不一致、
不正なサイズと値数を拒否する。``FragmentLut`` のCPU比較19件と不正入力9件を
両GPUで検証する。既存V1の40件と不正入力7件も再検証する。

IOをソースからOpenColorIO 2.6.0、OpenImageIO 3.1.18.1、FFmpeg 9.0.2、
SeExpr 2.11で再構築した。既存の22プラグインIDとバージョンを保持する。
旧バンドルの同梱DLLはバックアップし、共有の最新runtimeを優先する構成にした。
FFmpegの削除された ``AVCodec::pix_fmts`` は公開のsupported-config APIに置き換えた。
``tools/build-windows-ofx.ps1 -PluginSets io`` で再構築できる。

配布する6つのOCIOノードは、GPUを有効にするとVulkan shaderを使う。
OCIOの ``GPU_LANGUAGE_GLSL_VK_4_6`` とset 0のbinding情報からshaderを構築し、
1D / 2D / 3D LUT、std140のfloat / bool / vec3 / float配列 / int配列を転送する。
複数LUTと動的grade / curveにも対応する。processorとそのcache IDを保持し、
別processorの初期uniform値を誤って再利用しない。linear補間はGPU samplerの
丸め差を避けるため、シェーダー内のfloat計算とclampしたtexelFetchで評価する。
nearestとtetrahedralはOCIOが生成した計算を使う。

``GenericOCIOOpenGL.cpp`` と ``glsl.cpp`` はこのIOビルドから除外した。
GPU checkboxはnative suiteの有無で有効にし、CPU設定も保持する。
標準OFX clipは引き続きCPU画像であり、入力をVulkanへ転送して結果を戻す。
RGB / RGBA、負のrow stride、paddingとalphaを保持する。
ノード単位のcontext / shaderを再利用し、エラーはOFX persistent messageで通知する。

``tools/test-windows-vulkan-ocio.ps1`` はRTX 5080とAMD Radeon Graphicsを対象に、
独立したOFX C ABIモジュールからOCIOのCPU Float processorとの64件の比較を行う。
15 processor、4 worker、負のstrideとpadding、7個同時の3D LUT、
2Dに格納する大きな1D LUT、5種のdynamic uniformを検証する。
各sampleの許容値は絶対差 ``3e-5`` ＋参照値の絶対値× ``3e-5``。
HDR値では絶対差だけが ``3e-5`` を超えることがあるため、この式で判定する。

配布ノードの実レンダーでは6ノード、LUTの補間方式、premult、mask / inverted mask、
mix、Unicodeプロジェクトの再読込を含む15件をCPUと比較する。
各1247 pixel、GPUごとに18705 pixelを比較し、native呼出し回数も確認する。
``build/ocio-vulkan-tests/report.json`` に両GPUの結果、ソース / 実行ファイル /
runtime DLLのSHA256、終了後の成果物、stdin EOFとユーザー設定の一致を保存する。
テスト専用profileとPID watchdogを使い、起動・保存確認ダイアログによる停止を検出する。

新しいIOを使うGUIの回帰検証で、AMDのViewer生成時にWin32 surface関数を
取得できない初期化失敗が2回発生した。表示用instanceはQtの早期の列挙結果だけに
依存せず、active QPAの必須surface extensionを直接要求する構成に直した。
またVolkのdevice table生成で、global resolverを所有instanceの関数に
loader lock内で設定してから取得するよう修正した。
``Viewer`` テストにはheadless contextを先に作り、validation有 / 無のcontextを
4 workerで16個生成しながら表示を初期化する比較を追加した。
float画像の転送結果とvalidationを確認し、両GPUで通過した。
失敗時のログは ``build/vulkan-natron-ocio/gpu1/*/OverlaySmoke/failure-first*`` に保持する。
修正後のGUI結果は同じmatrixの通常のcheckpointに保存する。
さらにnative windowがキーを受け取ってもQWidget側のfocusが別のeditorを
指していると、後続のFocusOutでControlの状態が残るケースがあった。
キー入力を転送する前にViewerのwidget / overlay focusを同期するよう修正し、
既存のfocus喪失とmodal割り込みの編集テストで検証する。

修正後の実行ファイルでOCIO、Shadertoy、Engine、CTestと通常OFXの検証を取り直した。
GUIはViewer / overlay / DrawSuite / parameter DrawSuiteを両GPUの8u / 32fで
計16ケース検証し、すべて通過した。予期しないダイアログとvalidation errorは0件。
終了後のstderrも確認し、ユーザー設定のfingerprintは一致した。
``build/vulkan-ocio-validation-proof.json`` は各reportと成果物を独立に照合した
最終結果で、実行ファイル / IO / runtimeの一致も確認する。

次にEngineの通常GPU選択、配布プラグインの残るGL overlay、Viewerとの
logical device共有と追加画像形式を移植する。IOの色変換以外のoverlayと
他host向けGLコードは残っており、Vulkan完全移行はまだ完了していない。

Engine画像形式・色空間変換
--------------------------------------------------

``Image::convertToFormat`` / ``convertToFormatAlpha0`` にVulkan画像を接続した。
通常のCPU変換処理にGPU画像のnull pixel pointerを渡さず、同じcontext内の
Float RGBA変換はcompute shaderで実行する。Linear / sRGB / Rec.709 / BT.1886の
既存CPU式、alpha保持、負値とNaNの扱いを対応させた。
別のboundsを持つ入力はGPUコピーで座標を揃え、同じ画像への変換はGPUの一時画像を使う。
ROI外の画素と描画済みbitmapは保持する。

Byte / Short / Float、Alpha / XY / RGB / RGBAのCPU画像との境界は既存のCPU変換を
使う。VulkanからCPUへは画像のread lock下でsnapshotを作り、CPUからVulkanへは
変換したROIだけをuploadする。異なるcontext間もこの境界を明示的に通る。
native storageの形式は引き続きFloat RGBAで、Halfや追加GPU形式は未対応である。

``tools/test-windows-vulkan-image-convert.ps1`` は両GPUで各1218件をCPUと比較する。
CPU→Vulkan 384件、Vulkan→CPU 768件、GPU内64件、context間2件を含む。
Byte / Shortは完全一致、Floatは絶対差 ``3e-5`` ＋参照値の絶対値× ``3e-5`` で判定する。
負の画像原点、ずれたbounds、部分ROI、alias、alpha=0、unpremult、bitmap、
RoD / mipmap level / pixel aspect / premult / fieldingも確認する。
GPU内64件はcontextのupload / download counterが増えないことを確認する。
``build/vulkan-image-convert-tests/report.json`` はソース・実行ファイル・終了後ログの
SHA256とユーザー設定のfingerprintを保存する。
この変換処理は通常のGPUノード選択と画像キャッシュを接続するための前提であり、
この検証段階では通常のOFX clipはCPU画像を返す。

変更後のApp / RendererでEngineの660 compute参照＋95 Image/CacheEntry参照、
Shadertoy 19件、OCIO 64参照＋配布ノード15件を両GPUで再検証した。
Viewer / overlay / DrawSuite / parameter DrawSuiteのGUIは両GPU×8u / 32fの
16ケースを検証し、CTestと通常OFXのテストも通過した。
予期しないdialogとvalidation errorは0件、終了後もユーザー設定は一致した。
``build/vulkan-image-convert-validation-proof.json`` はreportと成果物を独立に照合した結果である。

次の実装単位
------------

EngineのNaN処理と共有ランタイム
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``Image::checkForNaNsAndFix`` と内部のread-only検査をcomputeへ接続した。
RGBAの全channelを検査し、NaNだけを1へ修正する。InfinityとROI外の画素は保持する。
read-only検査では画像を更新せず、shaderのatomic flagだけを返す。
storage bufferのGPU clear→compute→hostのbarrierを明示し、4 byteのflagをreadbackする。
画素全体のupload / downloadは使わない。

``Engine/VulkanImageContextPool.cpp`` は同じGPU選択とvalidation設定のruntimeを
共有する。OFX Image Suiteの ``createContext`` もこのpoolを使い、contextごとの
opaque handle namespaceとrelease規則は維持する。poolはweak referenceだけを保持し、
画像とactive contextが所有するlogical deviceを最後の所有者とともに解放する。
Viewerのpresentation deviceとの共有と、automatic / explicit選択の正規化は未接続である。

``ImageStorageSmoke`` は従来95件にNaN 8件、runtime共有3件を加えた106件を検証する。
NaNはRGBA各channel、検出有 / 無、readonlyのbit保持、alpha、負のゼロ、
正負Infinity、空ROI、修正後の再検査、bitmapをCPU参照と比較する。
runtimeは8 workerの同時取得、context identity、GPU内コピーと最後のimageの寿命を確認する。
NaN処理と共有runtimeの画像コピーではpixel transfer counterが増えないことも確認する。
修正後の画素はCPU参照とbit単位でも比較し、負のゼロ、InfinityとROI外のNaN payloadを確認する。
変更後のApp / RendererでEngine 106件、Compute 660件、Shadertoy 19件、OCIO 64参照＋15件、
画像形式変換1218件を両GPUで検証した。Viewer / overlayは両GPU×8u / 32fの8ケースを検証し、
CTestと通常OFXのテストも通過した。終了後のvalidation errorと予期しないdialogは0件で、
ユーザー設定は一致した。``build/vulkan-native-runtime-validation-proof.json`` に
独立に照合したソース・実行ファイル・成果物の結果を保存する。

通常のGPUレンダー接続
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``NatronVulkanRenderSuite.h`` にV1の借用APIとrender capability / action flagを追加した。
capabilityはframe開始時に捕捉する。初期接続は単一plane、full resolution、Float RGBAに限定し、
対応effectのキャッシュと一時出力をVulkan storageへ振り分ける。
標準OFX clipはnative actionでCPU dataをnull、rowBytesを0として返し、
プラグインの既存の所有contextへopaque image handleを貸し出す。
共有runtimeの一致を確認するため、画像と永続shader programは同じlogical deviceを使う。

借用recordはallocationとentry access guardを保持し、Image / Shader Suiteはそのstorageを
直接操作する。入力はread-only、必要なcropはGPUコピー、出力への書込みはrender ROIに制限する。
全操作で読み書きの範囲と権限を確認し、no-opやaliasも検査を迂回しない。
借用handleは標準clipのreleaseで全contextから失効する。``releaseImage`` では借用を解放できない。
プラグインが所有する一時画像と永続programは別の寿命を持つ。
clip / contextのrelease前にその画像を使うsuite callを完了させる契約である。

``Image::convertToStorage`` はCPU境界を明示し、同じruntimeのnative画像をそのまま返す。
metadata、bitmap、PAR、RoD、mipmap levelを保持する。キャッシュはresident native画像を
CPU snapshotより優先し、未完成の画像を異なるstorageで再レンダーしない。
CPU-only effectがnativeキャッシュを受け取る場合は完成済み画像をRAMへ変換する。

``ClipBorrowSmoke`` は両GPUで110件を検証する。負のorigin、部分ROI、権限制約、
context別namespace、clip release後の失効、contextを先に解放する順序、
永続shader再利用、bitmapとROI外の画素を確認し、borrow / compute中のpixel transferは0である。
独立したOFX C ABI moduleはReader→nativeノード2個→Writerを実レンダーする。
CPU clip経路との全画素比較、キャッシュ再利用、Unicode project再読込み後の実レンダーを検証する。
IOの色変換をLinear→Linearとして測ると、1 renderにつきReader境界のupload 1回と
Writer境界のdownload 1回のみで、中間nativeノードのCPU pixel transferは0である。
Reader / Writerに残るOCIOのCPU clip stagingは別の処理であり、OCIOテストで検証する。
この実装でそのstagingを除去したとは扱わない。

変更後のApp / Rendererで両GPUのEngine Image 106件、Compute 660件、
Clip borrow 110件、形式変換1218件、Shadertoy 19件、OCIO 64参照＋配布ノード15件を再検証した。
Viewer / overlayは両GPU×8u / 32fの8ケース、CTestと通常OFXのテストも通過した。
終了後のvalidation errorと予期しないdialogは0件で、ユーザー設定のfingerprintは一致した。
``build/vulkan-native-clips-validation-proof.json`` は当該検証時のソース・実行ファイル・
成果物を独立にSHA256で照合した結果である。

配布OCIO / Shadertoyのnative clip接続
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

配布OCIOの6ノードとShadertoyに借用APIを接続した。Float RGBAのnative actionでは
標準clipのCPU dataを使わず、プラグインの永続context / programから同じruntimeの
画像を操作する。OCIOのGPU無効化はinstance capabilityも更新し、次のframeで
CPU経路を選択する。descriptorもnative capabilityを宣言する。

OCIOはROIのGPUコピー、色変換、(Un)premult、Mask / Mix、出力へのコピーをGPUで行う。
CPU経路に合わせて、微小alphaのunpremultと非正 / NaN alphaのpremultを扱う。
Mixが0 / 1のときは選択した画素をそのまま保持し、他方のNaNやInfinityとの
ゼロ乗算を避ける。Alpha maskは現状のCPU形式を境界で1回uploadする。
Reader / Writerの内部OCIO、他のcomponents、mipmap / Half等の移植は残る。

Shadertoyはnative出力へ直接描画し、対応入力を借用samplerとして使う。
allocation boundsからfragment座標を求め、RoD基準のuniformと入力offsetを維持する。
標準CPU clipのpack / upload / download経路は未対応format / scaleの境界として残る。

CPU境界では要求したcomponents / depthに一致する完成済みsnapshotを再利用し、
Vulkan出力をRGBへ変換した後の重複レンダーを避ける。native形式変換はbitmapも保持し、
部分ROIの未完成画素を完成済みと扱わない。複数入力へ同じeffectを接続した場合も
入力ごとのframe / components / storage要求を保持する。Alpha clipの取得では
選択元レイヤーだけでなく返すclipの形式を確認し、マスクをVulkanへprefetchして
再度CPUへ読み戻す往復を避ける。

OCIOのテストは6ノードのpremult、共有RGBA / 独立Alpha mask、LUT / Mix、
2ノードのnative graph、cache再利用、Unicode project再読込みの22比較へ拡張した。
Shadertoyの19ケースもnative render counterとgraph全体のtransfer counterを照合する。
色画像はReader境界のuploadとWriter境界のdownload各1回、マスクは追加upload 1回を
期待する。2ノードのcache再利用ではnative再描画0回、upload 0回、Writerのdownload 1回を
期待する。Computeの6 endpoint比較を含めた件数は666件である。

変更後のApp / Rendererと配布IO / Miscを両GPUで再検証した。OCIO 64参照＋22比較、
Shadertoy 19ケース、Compute 666件、Engine Image 106件、Clip borrow 110件、
形式変換1218件、Viewer / overlayの両GPU×8u / 32fの8ケースが通過した。
CTestと通常OFXも通過し、終了後のvalidation errorと予期しないdialogは0件、
ユーザー設定は一致した。``build/vulkan-shipped-native-clips-validation-proof.json`` は
この検証時のソース・実行ファイル・成果物を独立にSHA256で照合した結果である。

残るmipmap処理、Halfや他のcomponents / formats、並列native graphとVRAM管理も移植する。
Shadertoyの残るpresetも実レンダーで検証する。
配布プラグインの残るオーバーレイもOFX DrawSuiteへ移し、編集を復元する。
Roto/Trackerの全ツール、tablet/focus/keyboard入力、変形・文字の細かな表示一致も
引き続き検証する。カーブとDope Sheetの細かな表示・編集一致、
Engine GPU画像処理、OFX GPUノード、
CPU/rasterフォールバック、非同期・VRAM管理も引き続き移植対象である。
本体の詳細な表示一致と編集操作の検証が残っており、段階0/1全体はまだ完了していない。

proxy表示のnative描画
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

render scaleに対応したeffectは、縮小率にかかわらずFloat RGBAのnative経路を選択する。
full scaleで描画してから縮小するeffectは従来の境界を維持する。
この検証時のnativeキャッシュは同じmipmap levelの完成済み画像を再利用した。
異なるlevelからのGPU縮小生成は、次項で追加している。

CPUのmipmap生成は奇数幅・高さの端も書き込み、存在する画素だけでbox平均を取る。
負の原点、1画素の行・列、1×1画像も同じ処理で扱う。
旧1D専用処理を廃止し、画素の取違えとbitmapの欠落を避ける。
縮小したbitmapは全寄与画素が完成している場合だけ完成とする。
``ImageMipmapTest`` はByte / Short / Float、9形状、3 level、bitmap有無の
162比較と、未完成の角のbitmapを確認する。

``tools/test-windows-vulkan-proxy.ps1`` は実際のViewerで等倍、1/2、1/4、
1/8、1/16、1/32を検証する。2ノードのnative test graphと配布OCIO graphを
CPU clip経路と比較し、nativeキャッシュへ戻した際の再描画0回も確認する。
Shadertoyは縮小率とRoDから求めた ``iRenderScale`` / ``iResolution`` を
shader内で照合する。OFX test graphはinput / output clipのrender scaleが
actionと一致することも確認する。各GPUの8u / 32fで18比較ずつ実行する。
GUIの入力変更に伴うabortable renderが収束してから現在の状態を表示し、
一時的に切断されたViewerの黒いframeを比較結果に使わない。
profile、PID watchdog、終了後の成果物照合、ユーザー設定のfingerprintを維持する。

両GPUの8u / 32fで72比較、frame上の30600 pixelが通過した。
CPU表示との最大差は1階調で、nativeキャッシュへ戻した際の再描画は0回。
32fのキャッシュ表示は完全一致し、8uは既存のLUTディザ処理による最大1階調の
差を許容する。native test graphの描画中のpixel transferは0回である。
変更後のApp / RendererでOCIO 64参照＋22比較、Shadertoy 19比較、Compute 666件、
Engine Image 106件、Clip borrow 110件、画像形式変換1218件を両GPUで再検証した。
Viewer / overlayの8ケース、CTestと通常OFXも通過した。
予期しないdialogとvalidation errorは終了後も0件で、ユーザー設定は一致した。
``build/vulkan-native-proxy-validation-proof.json`` に、この検証時のソース・
実行ファイル・成果物を独立にSHA256で照合した結果を保存する。

nativeキャッシュからのGPU mipmap生成
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

完成済みのnativeキャッシュから、より低い解像度のキャッシュをGPUで生成する。
Float RGBA画像を1 levelずつ2×2のbox平均で縮小し、画素のglobal座標を
元に負の原点と奇数サイズの端を処理する。端は存在する画素だけで平均する。
GPU画像の画素をCPUへ取り出さず、中間画像も同じruntimeに確保する。
bitmapはCPU側の描画状態として引き継ぎ、寄与画素がすべて完成した場合だけ
縮小画素を完成とする。対象ROIの外の画素とbitmapは維持する。
CPU / nativeとも中間画像の所有をshared_ptrに統一した。

同じlevelのCPU snapshotを優先する一方、異なるlevelから縮小する場合は
nativeキャッシュを優先する。要求したsource footprintが未完成、別runtime、
低解像度、形式不一致のnative画像は再利用しない。CPUへのdownloadは
CPU effect / Viewer等の境界で行う。

通常の等倍画像にも「低解像度入力から描画した等倍画像」のkeyが付いていたため、
proxy画像とのkeyが一致せず高解像度キャッシュが選ばれなかった。
このflagは、proxy要求を等倍actionで描画し、入力もproxyのままである場合に
限定した。等倍入力から得た通常の等倍画像は他の縮小率と同じkeyで再利用する。

``ImageStorageSmoke`` に360比較を追加した。9形状、2 source level、
full / partial ROI、5縮小率、bitmapの有無を組み合わせ、CPU値との一致、
paddingと未完成bitmapの保持、縮小中のpixel transfer 0回を検証する。
``VulkanProxySmoke`` は従来の18比較に、native / 配布OCIO graphの等倍cache
準備と1/2から1/32への10比較を加え、1実行30比較とする。
高解像度cacheからの縮小ではnative再描画とuploadを0回とし、Viewer境界の
downloadを確認する。

NVIDIA GeForce RTX 5080とAMD Radeon(TM) Graphicsで各360比較が通過した。
画像処理全体のCPU参照との差は最大1.19209e-7で、縮小中のpixel transferは0回。
両GPUの8u / 32f表示で120比較、51000 pixelが通過した。CPU表示との差は
最大1階調で、32fのcache表示は完全一致した。
高解像度native / 配布OCIO cacheからの40縮小比較はnative再描画0回、
upload 0回、Viewer境界のdownload 1回だった。
OCIO 64参照＋22配布比較、Shadertoy 19比較、Compute 666件＋16不正入力、
Engine Image 106件、Clip borrow 110件、画像形式変換1218件も両GPUで通過した。
GUI / overlay 8ケース、CTestとOFX project roundtripも通過した。
予期しないdialogとvalidation errorは終了後も0件で、ユーザー設定は一致した。
``build/vulkan-native-mipmap-validation-proof.json`` に、この検証時のソース・
実行ファイル・成果物を独立にSHA256で照合した結果を保存する。

この時点ではrender scaleに対応しないeffectの等倍actionとGPU縮小の接続が残っていた。

等倍専用OFXのnative描画
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

render scaleに対応しないFloat RGBA effectもnative画像で描画する。
proxy入力はglobal座標を基準にnearestでGPU拡大し、OFX actionとclipの
render scaleを1にそろえる。処理後はGPU mipmapで要求されたproxy levelへ戻す。
キャッシュ再利用時の縮小先もnative画像を確保し、中間画像をCPUへ転送しない。
CPU Reader等のproxy境界画像も元のkeyを保持する。等倍cacheの描画完了後に
source footprintの完成を確認し、local proxy出力のbitmapへ反映する。
事前取得した完成済み画像をclip取得時に再利用し、重複uploadを避ける。
hostが復元する未処理チャンネルの元画像も、必要なら同じGPU拡大を使う。
CPU Alpha maskは等倍へ拡大し、各host mask/mixの境界で1回uploadする。

CPU nearest拡大もROIとglobal座標からコピーするよう変更した。
奇数サイズと負の原点を処理し、出力がROIより広い場合もpaddingを上書きしない。
bitmapは従来どおり変更しない。CPU / native境界では対象ROIのuploadまたは
downloadを1回行い、native間の拡大ではpixel transferを行わない。

proxy入力から等倍描画した画像は、入力mipmap levelもcache keyへ含める。
1/2入力で作った等倍画像を1/32入力の結果として使わず、入力品質ごとに描画する。
等倍入力から作ったキャッシュは既存の高解像度cache縮小で再利用する。
``ImageKey`` のcache serializationをversion 3へ更新し、品質flagと入力levelを
保存する。これらが保存されていなかった旧cacheは品質不明として再利用しない。
project serializationとPython APIは変更しない。

``ImageUpscaleTest`` はByte / Short / Float、Alpha / RGBA、正負の原点、
2拡大率、full / partial ROIの48組合せで独立したnearest参照とpaddingを確認する。
``ImageStorageSmoke`` は9形状、2出力level、full / partial ROI、5拡大率、
native間 / CPU upload / CPU downloadの540比較を追加する。
``VulkanProxySmoke`` は等倍専用2-node chainについてproxy入力、等倍入力、
host channel / mask / mixと、入力品質を変えるcache / 等倍cache縮小を追加し、
1実行60比較とする。等倍action、CPU画像との一致、cache再利用とCPU転送を確認する。

グラフを切り替える際はViewerを停止・切断し、前のrender actionと転送counterが
落ち着いてから次のグラフの計測を始める。Reader metadataとproxy設定の変更も
切断中に行う。これにより、終了待ちのCPU参照処理のreadbackを次のnative graphの
転送量へ混ぜない。native / reference / cacheの各切替でこの確認を行い、
native graphとcache再表示でのゼロ転送条件を維持する。実行制限は1構成600秒である。

NVIDIA GeForce RTX 5080とAMD Radeon(TM) Graphicsで各540拡大比較が通過した。
native間180比較はpixel transfer 0回、CPU境界の各180比較はuploadまたは
download 1回だった。既存のGPU mipmap 360比較も通過し、Engine画像処理全体の
CPU参照との差は最大1.19209e-7だった。
両GPUの8u / 32f表示で240比較、102000 pixelが通過し、うち120比較が
等倍専用OFXだった。CPU表示との差は最大1階調、32fのcache表示は完全一致した。
通常の等倍専用chainはReader境界upload 1回、Viewer境界download 1回で、
2-node host mask/mix chainはAlpha mask upload 2回を加えてupload 3回だった。
入力品質を変えるcacheでは各品質でnative action 2回、再表示では0回だった。
高解像度native / 配布OCIO / 等倍専用cacheからの60縮小比較は
native再描画0回、upload 0回、Viewer境界download 1回だった。

OCIO 64参照＋22配布比較、Shadertoy 19比較、Compute 666件＋22不正入力、
Engine Image 106件、Clip borrow 110件、画像形式変換1218件も両GPUで通過した。
GUI / overlay 8ケース、CTestとOFX project roundtripも通過した。
予期しないdialogとvalidation errorは終了後も0件で、ユーザー設定は一致した。
``build/vulkan-native-fullscale-validation-proof.json`` に、この検証時の
ソース・実行ファイル・成果物を独立にSHA256で照合した結果を保存する。

Halfや他のcomponents、native graphの並列実行とVRAM管理、
Reader / Writer内部OCIO、Viewer deviceとの共有と残るGL経路の移植は継続する。
等倍専用OFXの実graphでの負原点、identityと複数ROI取得も別途確認する。

Reader内部のnative OCIO
-----------------------

Float RGBAの配布Readerは、デコードするCPUバッファを1回uploadし、
色変換とpremultiplicationをnative clipへ直接出力する。
Readerのinstance propertyを ``ReadNode::supportsVulkanRender()`` で転送し、
単一のRGBA color planeを処理するmulti-planar Readerもnative描画対象にする。
複数のplane、RGB / Alpha / XY、解析描画は既存のCPU経路を使う。
``Vulkan processing`` を無効にするとReaderの色変換もCPU参照になる。

Natronは従来どおりReaderを等倍でデコードしてcacheし、proxyをEngine側で
縮小する。解像度ごとの再デコードを増やすための変更は行わない。
``OcioVulkanProcessor::uploadReader`` はcropをupload後、unpremult / OCIO、
必要な縮小、最後のpremultの順で処理する。内部縮小はglobal pixel gridに
沿う2x2平均で、画像端では存在するpixelだけを平均する。
CPU側GenericReaderの複数段縮小では、一時バッファ未確保とaliasを解消し、
tileの余白を平均に含めず、負の原点でも範囲外pointerを作らない。

Writerに渡すCPU RGB snapshotは、Readerの拡張data windowを描画する
native cacheの代わりに使用しない。完成したnative画像のsnapshotは全boundsを
変換してbitmapを保持し、後続clipの範囲が広がっても同じdownloadを再利用する。
部分snapshotをRGBA専用native storageへ昇格することも防ぐ。

検証コマンド::

    tools/test-windows-vulkan-reader.ps1

専用profileとPID watchdogを使用し、ユーザー設定のSHA256を前後で照合する。
キャッシュentryはGPUメモリの割り当て前に他の描画スレッドから見える。
Vulkan contextを参照する前に ``allocateMemory()`` でentryの割り当てを同期する。
``ClipBorrowSmoke`` はこの状態の再現を含む115件を検証する。
テスト終了後は専用profileのViewer cache partsを削除し、設定・ログ・証跡を残す。

``ReaderEngineVulkanSmoke`` は216件の内部GPU縮小・crop・色変換・alpha比較、
signed / padded rows、出力ROI外のguard、入力buffer不変を検証する。
配布Readerの35比較は負の座標、HDR、ゼロ / 微小 / 負値alphaを含み、
CPU参照、実Reader→配布OCIO 2-node chain、project roundtripと転送回数を確認する。
``ReaderVulkanSmoke`` は7つの色 / alpha設定を6段階のproxyで比較し、
比較用Viewerを1400×600に固定して、入力切替後の表示倍率をそろえる。
同じnative Reader cacheの再表示で再デコード / uploadが増えないことを確認する。
GUIの操作で起動するabortable描画と明示的な再表示は別のViewer downloadになるため、
各decoder actionのuploadが1回であることを確認し、厳密な1回ずつの境界転送は
headless描画で検証する。Reader / WriterのCPU decoder / encoderは境界として残る。

2026-10-11のcheckpointではRTX 5080 / AMD Radeon Graphicsの双方で、
内部処理216件（296832 channel samples / GPU）、配布Reader 35比較 / GPU、
8u / 32f表示の42比較 / 条件、合計168表示比較（71400 pixel samples）が通過した。
CPU参照との最大絶対誤差は5.72205e-5で、abs + relativeの許容範囲内である。
Viewerの最大差は1階調、resident Readerの再デコード / uploadは0回、
処理中のCPU downloadも0回である。project roundtripとnative OCIO 2-node chainも通過した。
取得画像504枚はすべて1400×600で、Reader証跡1016ファイルのハッシュを照合した。

既存のproxy 240比較、full-scale fallback、OCIO、Shadertoy、Engine Image、
Clip borrow 115件 / GPU、形式変換1218件 / GPU、GUI / overlay 8条件、
CTest、OFX inventory 22ノード / project roundtripも再検証した。
確認ダイアログ、Vulkan validation errorは0件で、ユーザー設定の指紋は不変である。
終了済みテストのViewer cache partsは削除される。
統合証跡は ``build/vulkan-native-reader-validation-proof.json`` で、
ソース・実行ファイル・生成画像・設定のハッシュを独立して検証した。
このcheckpointのApp / Renderer / IOのSHA-256はそれぞれ
``0661e41deaa482964765158f94a22c801a940902075e041d8c8b3a7c416bf192``、
``650ac824c8ba39d2dea96e778ffc9d7d2fea40fd74f3d12e9016398af622c57e``、
``ddc6d52dfcbe2ecdd814d53f594d0383b5573314f70645edb66c21b7c23b6674`` である。

以下のWriter checkpointでFloat/RGBAのWriter内部OCIOもnative clipへ移行した。
他のcomponent/depth形式への拡張は引き続き移行対象である。

Writer内部OCIOとエンコード境界
------------------------------

``GenericWriter`` はFloat/RGBAのnative入力を取得し、GPU上の一時画像へROIを
コピーして、opaque / unpremult、OCIO、premultの順に処理する。alphaを保存できない
形式については既存CPU処理の順序に従う。入力clipを書き換えず、エンコード用の
CPUバッファへ最後に一度だけdownloadする。チャンネルpackingとファイルcodecは
既存のCPU処理を使う。Output clipには色変換前の入力をGPU上でコピーするため、
保存ファイル用の色変換が後続ノードへ漏れない。

``WriteNode`` は埋め込みWriterのnative対応を伝え、保存時のschedulerは利用しない
最終出力をCPUへ再転送しない。途中のWriterについても、同じrender actionで
prefetch済みの完全な画像を再利用する。これによりclip取得のために同じファイルを
二度エンコードすることを防ぐ。別のrender actionではWriterのglobal cacheで
エンコードを省略しない。ファイル削除後の同一フレーム再描画でも両Writerが保存する。

EXR Writerの既存CPU経路も修正した。floatチャンネルのバイト位置、half行バッファの
寿命、最終列のコピー、負のoriginを持つ行の参照、display windowのinclusive終点を
修正し、``Slice::Make`` で行のoriginとstrideを渡す。CPU/GPUの一致だけでは同じ
破損を見逃すため、identity EXRを元の入力画素とも独立に比較する。

検証は次のコマンドで再現できる。

.. code-block:: powershell

   uv run --frozen python tools/reader-vulkan-fixtures.py
   tools/test-windows-vulkan-writer.ps1

2026-10-11のRTX 5080 / AMD Radeon Graphicsでは各GPUで次を確認した。

* EXR 16/32bit、PNG、float TIFF、JPEGの150組と、CPUとの境界2組、通常のWrite
  コンテナを含むグラフ2組、計154組を比較した。各GPUで730,742チャンネルを比較し、
  25組のidentity EXRを入力画素とも照合した。PNG/JPEGは両GPUで画素値が一致した。
* 240内部ケースでは69,120サンプルに加えて、入力の不変性、Outputの元画像、
  ROI外の保護を検査した。符号付きのpadded row、負の座標、alpha 0 / epsilon /
  負値も含む。最大絶対誤差はRTX ``5.57899e-5``、AMD ``5.67436e-5`` だった。
* Reader→Writeコンテナ→Writerのnativeグラフはupload 1回、download 2回だった。
  同一フレーム再描画はReaderのGPU画像を再利用し、upload 0回、download 2回で
  両ファイルを再生成した。プロジェクト再読込でもGPU設定と元画像の受け渡しを維持した。
* テスト専用profile、PID watchdog、interpreterのstdin EOFを使い、起動・実行・
  終了待ちを検出した。保存画像とログ630ファイルを終了後にhashで確認した。
  ユーザー設定のfingerprintは変わらず、Vulkan validation errorは0だった。

更新後のビルドでReaderの70保存比較、168 Viewer比較、504枚の1400x600 captureも
再検証した。最大Viewer誤差は1byte stepだった。Engineの115 clip/cache検査と
106 image検査、OCIOの64内部比較と22実プラグイン比較を両GPUで再実行し、CTestと
OFXプロジェクトroundtripも通過した。入力・binary・保存artifactのhash、stdin EOF、
終了後ログと設定fingerprintを独立に照合した結果は
``build/vulkan-native-writer-validation-proof.json`` に保存する。

このWriter checkpointのApp / Renderer / IOのSHA-256はそれぞれ
``13e8ed380e0fdc9d8593d1819981f0c41d9637c8217cc2957ec6c00d92c0de07``、
``58b21e1e26e655c989d0f74cba393a097d35e5e64fac67715747fc1d81c19449``、
``3483a9b5d31ce26f6fddfb2bace87b616c80c96838aa12475f155e9ec950b24c`` である。

詳細は ``build/vulkan-native-writer-tests/report.json`` に保存する。Half/RGB/Alpha/XYの
native clip、複数plane、ViewerとEngineのdevice共有、OFX overlayと残りのOpenGL経路は
このWriter checkpoint時点での完全移行の残作業である。

ViewerとEngineの共有deviceとGPU画像転送
---------------------------------------

``VulkanImageContextPool`` はphysical deviceの選択を解決してからcontextを検索する。
automatic選択、環境変数による選択、明示的な同一indexは同じlogical deviceを使う。
同じvalidation modeの複数physical deviceもinstanceを共有する。所有者はcontext、
image、Viewerで、poolはweak参照のみを保持する。

共有instanceは利用可能なWSI拡張を有効にし、Qtは ``setVkInstance`` で既存instanceを
借用する。deviceはsurface作成前でもswapchain拡張を有効にでき、Canvasを作る際に
実際のsurfaceのpresentation対応を確認する。surface拡張を持たないヘッドレスの
instanceでは、依存するswapchain拡張を有効にしない。

Float/RGBA、RGBチャンネル表示、linearの入力色空間、自動コントラストなしの通常表示は、
EngineへVulkan画像を要求する。RAMのViewer texture cacheを経由せず、Engine画像の
cacheを再利用し、表示領域をViewer所有の画像へGPU上でコピーする。表示画像が元画像の
後続書き込みを参照し続けないようにする。opaque表示のalphaもGPU上で1に置換し、
元画像のalphaは変更しない。ROI、負のorigin、PAR、mipmap levelと表示用gain/gamma/LUTを
維持する。色の取得やヒストグラムがCPU画素を要求した時だけsnapshotを作り、同じ
表示level内の後続取得で再利用する。GPU表示時にはペイントのpartial更新用に保持した
古いRAM画像の参照を解除し、表示経路の切り替え後に古い画像を再利用しない。

再現には次のコマンドを使う。

.. code-block:: powershell

   tools/test-windows-vulkan-engine.ps1 -ReportDirectory build/vulkan-native-viewer-engine-tests
   tools/test-windows-vulkan-reader.ps1 -ReportDirectory build/vulkan-native-viewer-reader-tests

2026-10-12のRTX 5080 / AMD Radeon Graphicsでは、各GPUの8bit / 32bit floatで
42ケースずつ、計168ケースと504枚の1400x600 captureを確認した。32bit floatの
84ケースは入力画像のCPU downloadが0回で、cache再表示はupload / downloadとも
0回だった。CPU処理との最大画素差はRTXで1階調、AMDで0階調だった。146ケースの
native clip / GPU snapshot検査では、選択方法の正規化、instance共有、ROI外の保護、
元画像の後続書き込みからの独立性、NaN alphaを含むopaque処理を確認した。
GPU画像からのヒストグラムと色の取得、複数Viewer、surface作成前のdevice共有も
両GPUで通過した。検証中のユーザー設定fingerprintは変わらず、validation errorは0だった。

同じ最終ビルドでEngine、Reader、OCIO、GUI、Writer、CTest、OFXの回帰検証も通過した。
Writerは両GPU合計308ケース、1,461,484チャンネル値を独立した画像読み込みで照合し、
630個の保存成果物とログのhashを確認した。統合検証記録は
``build/vulkan-native-viewer-validation-proof.json`` に保存した。GUIの専用profile、
起動・終了ログとwatchdog結果も確認し、ユーザー設定は変更しない。

Reader GUIテストではプロジェクトの ``autoPreviews`` を無効にし、ノードグラフの
CPU thumbnail生成をViewerの転送量から切り分ける。通常のユーザー設定は変更しない。
RGB以外のチャンネル表示、自動コントラスト、partial更新、nonlinearの
入力色空間、ノードプレビューのGPU化は引き続き移植対象である。Half/RGB/Alpha/XY、
複数plane、VRAM制御、OFX overlayと残りのOpenGL経路も完全移行の範囲に残る。

8bit ViewerのGPU変換
--------------------

linearのFloat/RGBA入力をRGB表示する通常経路は、8bit表示でもEngineのVulkan画像を
直接使う。``ViewerDisplayConversion`` がgain / offset、ガンマ補間、表示LUT、alphaの
量子化をGPU上で適用し、表示用RGBA8画像を生成する。Float表示の変換順とは区別し、
従来の8bit処理と同じくガンマを表示LUTより先に適用する。

表示LUTはNatronのfloat上位16bitをindexとする65,536要素の表を使う。
ガンマは既存処理と同じ1,024要素を補間する。変更時に渡すのはこれらの表で、入力画素の
CPU downloadや表示画素のCPU uploadは発生しない。opaqueのalphaはGPU上で255にし、
元のEngine画像は変更しない。

各行の双方向error diffusionもGPUへ移した。開始位置は行とROIの原点から決定し、
キャッシュ再表示でランダムなノイズが変わらないようにする。CPU処理のランダムな
開始位置とは異なるため、ディザの違いは最大1階調として検査する。

表示サンプリングは共通のcanonical pixel gridを使う。最近傍の画素選択とbilinearの
重みをglobal grid上で求めてからtexture原点を引き、RAM cacheの大きなtileと小さな
GPU snapshotの丸め誤差を揃える。snapshotの外を補間する際は0を使い、従来のtileの
未描画領域を再現する。raster overlayなどpixel gridを指定しない入力は従来の
clamp samplingを維持する。

再現には次のコマンドを使う。

.. code-block:: powershell

   tools/test-windows-vulkan-viewer-display.ps1 -ReportDirectory build/vulkan-byte-viewer-display-tests
   tools/test-windows-vulkan-reader.ps1 -ReportDirectory build/vulkan-byte-viewer-reader-tests

表示設定のGUI matrixは4つのLUT、露出、gamma 0 / 0.45 / 1 / 2.2、straight /
premultiplied、proxy level 0 / 2 / 5を含む。専用test profile内だけで
``NATRON_TEST_VIEWER_CPU_REFERENCE`` を使い、CPU表示経路と同じviewportで比較する。
CPU比較中にnative転送のカウンタが増えないことも確認する。

2026-10-12の8bit移植時のビルドでは両GPUの表示設定32ケースずつが通過し、CPU表示との差は
最大1階調、cache再表示との差は0だった。バックエンドは各GPUで432ケースと
580,032チャンネル値を確認し、元画像の不変性、NaN / infinity alpha、ROIと安定した
ディザも通過した。共通pixel gridは各GPUの8フレーム、26,880,000チャンネル値で、
cropped / padded画像の全画面差が0だった。

Readerの8bit / Float表示も各GPUで42ケースずつ、計168ケースと504枚のcaptureが
通過した。入力画像のCPU downloadは両depthで0回、cache再表示のupload / downloadは
ともに0回で、再表示の画素差も0だった。8bitのCPU参照との差は両GPUとも最大1階調、
FloatはRTXで1階調、AMDで0階調だった。表示設定の192枚と合わせ、696枚の
1400x600 captureを保存して比較した。

同じビルドでEngine、8bit / FloatのGUIとヒストグラム・色取得、OCIO、Writer、
CTest、OFXの回帰検証も通過した。Writerは両GPU合計308ケース、1,461,484チャンネル値と
630個の成果物を照合した。入力ソース・実行ファイル・各レポートと成果物のhashを
独立検証した統合記録は ``build/vulkan-byte-viewer-validation-proof.json`` に保存した。
ユーザー設定のfingerprintは検証前後で一致し、validation errorは0だった。

2026-10-12に8bit移植時の実行ファイルと配布OFXも再監査した。Natron / NatronRendererの
直接importにOPENGL32.dllはないが、Arena / IO / Miscにはまだ存在する。
Vulkan ViewerのReader GUIテスト中のプロセスにもopengl32.dllがロードされていた。
``build/vulkan-opengl-retirement-audit.json`` に現在のbinary hashと実行時moduleの
記録を保存した。これはIOの操作ハンドル移植前の記録である。ViewerのGPU変換がVulkanで動くことと、アプリ全体のOpenGL依存が
除去されたことは別の完了条件であり、後者はまだ満たしていない。

IOの操作ハンドルとOpenGLリンクの撤去
-------------------------------------

IOの矩形、位置、変形、ランプの共通ヘルパーは標準OpenFX DrawSuiteでcanonical
座標の線・面・文字を出力する。影はpixel scaleから求め、変形は小さな2D行列で
合成する。GL行列、GL state、bitmap fontへの問い合わせは使わない。線幅はホストが
画面密度を補正し、点の輪郭と影もlogical pixel scaleを使う。位置パラメーターに
Natronの標準ハンドルがある場合は、従来どおりホスト側の操作を優先する。

``NatronOverlayInteract.h`` はDrawSuiteのentry pointを登録し、V1のGL entry pointを
空にする。旧OCIOレンダリング分岐、GLのcompile defineとlink、GLAD、旧文字描画の
objectをIOのビルド経路から除いた。SeGrainのコピーされた変形・ランプinteractは、
必要なパラメーターを同プラグインが定義していないため登録自体を撤去した。
画像処理と既存のパラメーターは維持する。

SupportExtは別のGit submoduleなので、変更は
``build-configs/windows-ofx/openfx-io-supportext-vulkan.patch`` に保存する。
``tools/build-windows-ofx.ps1`` が本体とSupportExtの両方へpatchを適用し、
``tools/verify-io-vulkan-source.py`` がpinとpatchの完全一致を検証する。

GUI検証中に見つかった設定パネルとUndo/Redo actionの寿命の問題も修正した。
破棄されるパネルをregistryから除き、色取得中のパネル参照と現在のactionはQPointerで
保護する。テストの終了は ``QApplication.exit`` を使い、保存確認を経由しない。
専用profile、起動marker、所有PIDのtimeout監視は起動確認などの予期しないdialogも
診断し、ユーザーの設定には触れない。

.. code-block:: powershell

   tools/test-windows-vulkan-io-overlay.ps1
   uv run --frozen python tools/verify-io-vulkan-source.py

2026-10-12のIOビルドにはOPENGL32.dllの直接importがない。26項目の数値描画検証と、
RTX / AMDそれぞれ画面倍率1 / 2での4種類のGUIハンドル検証が通過した。負の矩形原点、
回転・skew、点と輪郭、ドラッグ、focus後のmodifier解除、Undo/Redoを確認し、52枚の
captureを ``build/io-overlay-vulkan-tests`` に保存した。Arena / MiscのGLリンクと
EngineのGL初期化・レンダリング経路の撤去は引き続き必要である。

同じビルドのReader表示168ケース、OCIO各GPU22ケースと参照比較64ケース、Writer計
308ケース、CTest、通常GUI、標準DrawSuite、OFXの回帰検証も通過した。Readerの入力
CPU downloadは0回、Writerの630成果物と1,461,484チャンネル値を照合した。
ソースpin、保存patch、実行ファイルとレポート、成果物のhash、設定fingerprintを
独立検証した記録は ``build/vulkan-io-opengl-retirement-proof.json`` に保存している。

Misc / Arenaと画像コーデックのGL依存撤去
----------------------------------------

共通DrawSuiteヘルパーをMiscとArenaにも適用した。矩形・変形・ランプ・位置のほか、
CornerPin、TrackerPM、ContactSheetの固有interactもcanonical座標の線・面・文字を
出力する。GLのstateや行列への問い合わせは使わず、描画はV2 entry pointに登録する。
ArenaのTextとMagick Text、OCLの変形ヘルパーも同じ経路を使う。旧font、GLAD、
GL text objectとOpenGL linkを両bundleのactive targetから除いた。

ShadertoyのOpenGL / Mesa renderer、context管理と旧GPU選択パラメーターの分岐も
active sourceから撤去した。Vulkan suiteを持たないホストには不足するsuiteを返す。
既存のnode ID、native側のパラメーターとpresetは維持する。

Misc本体の変更は ``openfx-misc.patch`` に統合し、以前のincremental patchは撤去した。
各bundleのSupportExt変更は独立した ``*-supportext-vulkan.patch`` に保存する。
``verify-native-ofx-source.py`` は本体のpin、submoduleのgitlink、保存patchと実際のdiffの
完全一致を確認する。素のpinからpatchを再適用してソースが一致することも検証した。

Engine起動時のGL関数・platform context初期化と、フレームごとのGL context取得を
撤去した。旧GL設定を非表示にし、graphics backend欄はVulkanを表示する。
Aboutのライブラリ情報もVulkan physical deviceとAPI versionを問い合わせる。
続くEngineソース撤去で、GL image / render / storageの旧分岐とGLAD / platform context
source自体も取り除いた。現在の構成は下記「EngineのOpenGLソース撤去」を参照する。

TrackerPMの実操作で見つかったUndoの問題も修正した。image評価を行わないパラメーターも
Undo履歴へ記録する。新規keyと既存keyの更新を区別し、merged commandで同じkeyを
繰り返し更新した場合は最初の状態へ戻す。最後のkeyを削除するときは元のstatic値も
復元する。テストは実際のEdit menuを使い、レンダー停止によるvalue commitの遅延を
イベントループへ戻って待つ。

複数parameterのUndoをまとめるmapは ``std::owner_less`` 順だが、旧実装のsubset判定は
raw pointer順を使っていた。配置順によって、変化のないsize編集だけが別commandとなり、
Undoしても位置が戻らないことを実操作とcommandの記録で確認した。
subset判定はmap自身の ``find`` に統一する。static編集の比較と初期snapshotもGUIの値を
使用し、レンダー完了までqueueに保持される編集値を取り落とさない。
interactが別edit blockに送る変更のないhelper値はobsolete commandとして履歴に残さない。
明示的なkey編集は、値が同じ場合もkey追加などの状態変更を含むため維持する。

MSYS2のlibultrahdrにはEGL依存があったため、同じ1.5.1をGLES無効で再構築した。
DLL名と既存ABIを保ち、OIIOとImageMagick UHDR coderが必要とする30個のexportを
確認した。HLG / PQの入力からSDR 8bit、HLG 10bit、linear halfを復号する計6ケースは、
既存CPU実装と18,432画素およびJPEGの全byteが一致した。native版の実行プロセスには
OpenGL / EGL / GLES DLLのロードがない。

OFXビルドはcodecも自動構築し、runtime installerはsource-built DLLを優先する。
全bundleとImageMagick coderの依存を辿ってから不要なEGL / GLES DLLをbinの外へ
移動する。現在のruntimeは184 DLLで、applicationとcodecを含む332 PE fileの通常・
delay importにはOpenGL / EGL / GLESがない。

GUIで観測されたopengl32.dllのロードは、Vulkan loaderからNVIDIA driverを経由して
いた。moduleの存在だけではapplicationのGL API呼び出しは証明できない。
テストはmoduleを記録し、source / importの監査とVulkanの実描画を別々に検証する。
OBSのimplicit Vulkan layerは専用test process内だけで無効化し、呼び出し元の
環境変数を終了時に復元する。ユーザーのOBS設定には触れない。

.. code-block:: powershell

   tools/test-windows-vulkan-plugin-overlay.ps1
   tools/test-windows-vulkan-io-overlay.ps1
   uv run --frozen python tools/verify-native-ofx-source.py
   uv run --no-project --python .venv/Scripts/python.exe --with pefile==2024.8.26 python tools/test-windows-ofx-codecs.py
   uv run --no-project --python .venv/Scripts/python.exe --with pefile==2024.8.26 python tools/audit-native-ofx-runtime.py

EngineのOpenGLソース撤去
------------------------------

``GLShader``、``GPUContextPool``、``Texture``、``OSGLContext`` と全platform実装、
GLADのdebug / release source、旧context testと生成scriptを撤去した。
Engine image / cacheのGL texture保有、upload / download、FBO、shaderによる
copy / mask / mixと、render actionへのGL context受け渡しも除去した。
Vulkan image storage、GPU変換、CPU処理とRAM / disk cacheを使用する。
LinuxのEGL / GLX / Wayland context依存とApple OpenGL includeもビルドから外した。
これらのplatformはソース変更のみで、実機ビルドは未検証である。

HostSupportは ``OFX_SUPPORTS_OPENGLRENDER`` を定義しない。
OFX host propertyはOpenGL renderingを ``false`` とし、OpenGL Render Suiteの
取得にはnullを返す。GL texture clip、context attach / detachとresource flush経路は
削除した。配布pluginはVulkan suite / DrawSuiteとCPU image経路を使用する。
外部のGL専用pluginは使用できない。

旧project / preferenceのserialized keyとcacheの ``TexTarget`` fieldは読込互換性のため
維持する。storage enumの旧GL値3は再利用せず、Vulkanの値4を保った。
旧GPU選択欄は非表示で、node情報はVulkan対応を表示する。
``ViewerGL`` と ``OpenGLViewerI`` の名称も残るが、実装はVulkanである。

native Viewer画像はnode image cacheに保存されるため、Timelineはそのresident entryも
参照してcache markerを表示する。入力hash / plane / mipmap / ROIと描画済bitmapを照合し、
参照用metadataだけを保持する。Timelineを描くためのpixel downloadや画像所有は行わない。
per-node cache消去、再描画と入力切り替えでmarkerが更新されることを実操作で確認する。

``audit-native-engine-source.py`` は所有するC++ source、撤去済25ファイルと現在のNinja
buildを監査する。 ``audit-native-ofx-runtime.py`` はapplication / plugin / codecの通常・
delay importを別に確認する。SDKに残る標準OFXのGL宣言や、NVIDIA Vulkan driver内部の
DLL loadはapplicationのGL描画経路として数えない。

.. code-block:: powershell

   uv run --frozen python tools/audit-native-engine-source.py
   uv run --no-project --python .venv/Scripts/python.exe --with pefile==2024.8.26 python tools/audit-native-ofx-runtime.py
   tools/test-windows-vulkan-engine.ps1
   tools/test-windows-vulkan-image-convert.ps1
   tools/test-windows-vulkan-proxy.ps1
   tools/test-windows-vulkan-natron.ps1

参照資料
--------

APIの状態は2026-10-12に公式資料で確認した。

* `Qt 6.11 QVulkanInstance <https://doc.qt.io/qt-6.11/qvulkaninstance.html>`_
* `Qt 6.11 QVulkanWindow <https://doc.qt.io/qt-6.11/qvulkanwindow.html>`_
* `Qt 6.11 QRhiWidget <https://doc.qt.io/qt-6.11/qrhiwidget.html>`_
* `QWidget window container <https://doc.qt.io/qt-6.11/qwidget.html#createWindowContainer>`_
* `Vulkan versions <https://docs.vulkan.org/guide/latest/versions.html>`_
* `Vulkan lost device specification <https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device>`_
* `shaderc API <https://github.com/google/shaderc/blob/main/libshaderc/include/shaderc/shaderc.h>`_
* `Vulkan dynamic rendering <https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/extensions/dynamic_rendering/README.adoc>`_
* `OpenFX GPU API <https://openfx.readthedocs.io/en/main/Reference/api/file/ofxGPURender_8h.html>`_
* `OpenFX Interacts / DrawSuite <https://openfx.readthedocs.io/en/main/Reference/ofxInteracts.html>`_
* `OpenFX parameter interact properties <https://github.com/AcademySoftwareFoundation/openfx/blob/main/include/ofxParam.h>`_
* `OpenFX parametric background API <https://openfx.readthedocs.io/en/main/Reference/api/file/ofxParametricParam_8h.html>`_
* `OpenColorIO 2.5 Vulkan support <https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_5.html#vulkan-support>`_
* `OpenColorIO 2.6 shader API <https://opencolorio.readthedocs.io/en/v2.6.0/api/shaders.html>`_
* `OpenEXR framebuffer layout <https://openexr.com/en/latest/ReadingAndWritingImageFiles.html>`_
* `MoltenVK <https://github.com/KhronosGroup/MoltenVK>`_
* `Apple OpenGL deprecation <https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_intro/opengl_intro.html>`_
* `libultrahdr 1.5.1 build options <https://github.com/google/libultrahdr/blob/v1.5.1/docs/building.md>`_
