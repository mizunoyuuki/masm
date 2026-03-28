Ready to code?

 Here is Claude's plan:
╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌
 masm (mizuno assembly) — 自作アセンブラ開発計画・調査結果

 Context

 自作コンパイラが出力するx86-64アセンブリ（Intel構文）を、Linux上で動作するELFバイナリに変換する自作アセンブラ「masm」を開発する。
 実装言語はC言語。このドキュメントは開発に必要な知識、開発フロー、参考リソースをまとめた調査結果である。

 ---
 1. 必要な知識領域

 A. x86-64 命令エンコーディング

 アセンブリのニーモニック（mov, add, jmp等）を実際のバイト列に変換するためのルール:

 - REX プレフィックス: 64ビットレジスタ拡張用。0100WRXB の1バイト
   - W: 64ビットオペランドサイズ / R: ModR/Mのreg拡張 / X: SIBのindex拡張 / B: ModR/MのR/M拡張
 - ModR/M バイト: オペランド（レジスタ/メモリ）を指定。mod(2bit) + reg(3bit) + r/m(3bit)
 - SIB バイト: Scale-Index-Base形式のメモリアドレス指定。scale(2bit) + index(3bit) + base(3bit)
 - 即値・ディスプレースメント: 命令末尾に付加される定数値
 - オペコード: 命令本体（1〜3バイト）

 B. ELF64 バイナリフォーマット

 出力ファイル形式であるELF (Executable and Linkable Format) の構造:

 - ELF ヘッダ (64バイト): マジックバイト \x7fELF、アーキテクチャ、エントリポイント等
 - セクションヘッダテーブル: .text(コード), .data(データ), .bss(未初期化データ), .symtab(シンボル), .strtab(文字列),
 .rela.text(再配置) 等
 - プログラムヘッダテーブル: 実行時のメモリロード情報（LOAD, DYNAMICセグメント等）
 - シンボルテーブル: ラベル名→アドレスの対応表
 - 再配置テーブル: リンカが解決すべき参照情報

 C. アセンブラの内部アーキテクチャ（2パス方式）

 - 字句解析 (Lexer): ソースコードをトークン列に変換
 - 構文解析 (Parser): トークン列をAST（命令、ラベル、ディレクティブ）に変換
 - 第1パス: ラベルの収集、シンボルテーブル構築、各命令のサイズ計算
 - 第2パス: シンボルテーブルを使ってアドレス解決、マシンコード生成、再配置レコード生成
 - ELF出力: ヘッダ・セクション・シンボルテーブル等をバイナリとして書き出し

 ---
 2. 推奨開発フロー

 Phase 1: 最小限のELF出力

 - ハードコードした mov rax, 60; mov rdi, 42; syscall（exit(42)）のバイト列をELFファイルとして出力
 - ELFヘッダ・プログラムヘッダの構造を理解する
 - 生成バイナリが実行できることを確認

 Phase 2: 字句解析・構文解析

 - アセンブリソースのトークナイザを実装
 - ラベル、命令（ニーモニック+オペランド）、ディレクティブ（.text, .data, .global等）をパース

 Phase 3: 基本的な命令エンコーディング

 - まず少数の命令（mov, add, sub, push, pop, ret, call, jmp, syscall）から始める
 - レジスタ-レジスタ、レジスタ-即値の基本パターンを実装
 - GAS/NASMの出力と比較して正しさを検証

 Phase 4: シンボル解決（2パス実装）

 - 第1パスでラベル収集、シンボルテーブル構築
 - 第2パスで前方参照を含むアドレス解決
 - ジャンプ・コール命令のオフセット計算

 Phase 5: メモリオペランド対応

 - ModR/M + SIB による複雑なメモリアドレッシング
 - [rax + rbx*4 + 8] のような形式への対応

 Phase 6: セクション・再配置

 - .text, .data, .bss セクション分割
 - 再配置レコード生成（R_X86_64_PC32, R_X86_64_64等）
 - オブジェクトファイル(.o)出力→システムリンカ(ld)でリンク可能に

 Phase 7: 拡張

 - ディレクティブ拡張（.byte, .word, .quad, .ascii, .asciz, .align等）
 - 条件ジャンプ全般、比較命令
 - エラーメッセージの充実

 ---
 3. 参考リソース

 公式仕様書

 - Intel SDM (Software Developer's Manual): https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
   - Vol.1: 基本アーキテクチャ / Vol.2: 命令セットリファレンス（エンコーディング詳細）
 - System V ABI AMD64: https://refspecs.linuxbase.org/elf/x86_64-abi-20210928.pdf
   - 呼び出し規約、再配置タイプ
 - ELF仕様: https://man7.org/linux/man-pages/man5/elf.5.html

 英語リソース（チュートリアル・解説）

 - x86-64命令エンコーディング入門: https://www.systutorials.com/beginners-guide-x86-64-instruction-encoding/
 - x86-64エンコーディング実例: https://pyokagan.name/blog/2019-09-20-x86encoding/
 - ELFフォーマット チートシート: https://gist.github.com/x0nu11byt3/bcb35c3de461e5fb66173071a2379779
 - 手作業でELFを作る: https://medium.com/@dassomnath/handcrafting-x64-elf-from-specification-to-bytes-9986b342eb89
 - OSDev Wiki - x86-64命令エンコーディング: http://wiki.osdev.org/X86-64_Instruction_Encoding
 - OSDev Wiki - ELF: https://wiki.osdev.org/ELF
 - 2パスアセンブラの解説: https://kestrelcomputer.github.io/kestrel/2014/12/31/two-pass-assembler
 - アセンブラを作る (Go): https://rogchap.com/2019/07/28/building-an-assembler/
 - 再配置生成の詳細: https://maskray.me/blog/2025-03-16-relocation-generation-in-assemblers

 日本語リソース

 - アセンブラをゼロから作って自作コンパイラをアセンブルするまで (Zenn/dqneo): https://zenn.dev/dqneo/articles/012faee0b220fa
   - Go言語で自作アセンブラを実装した詳細な記事。自作コンパイラとの連携も解説
 - 低レイヤを知りたい人のためのCコンパイラ作成入門 (Rui Ueyama): https://www.sigbus.info/compilerbook
   - コンパイラ全般だがアセンブリ生成・ELF理解に有用
 - Hello Worldくらいしかできない言語のアセンブラをつくる (Qiita): https://qiita.com/mitsuchi/items/767544d6ca044cbcc9c4
 - リンカーを自作した (tyfkda): https://tyfkda.github.io/blog/2022/03/13/elf-linker.html
 - 自作Cコンパイラでセルフホスティングを達成した (tyfkda): https://tyfkda.github.io/blog/2019/07/14/self-hosting-c.html

 参考になるOSSプロジェクト

 - minias (C言語、教育目的の小さなx86-64アセンブラ): https://github.com/andrewchambers/minias
 - skx/assembler (Go言語、基本的なx86-64アセンブラ): https://github.com/skx/assembler
 - PeachPy (Python埋め込みx86-64アセンブラ): https://github.com/Maratyszcza/PeachPy

 検証ツール

 - オンラインx86-64アセンブラ/逆アセンブラ: https://defuse.ca/online-x86-assembler.htm
   - 自分のエンコーディング結果との比較に便利
 - objdump / readelf: 生成したELFの検証に使用
 - ndisasm: バイト列の逆アセンブルで正しさを確認

 ---
 4. 検証方法

 各フェーズで以下の方法で検証:

 1. Phase 1: chmod +x output && ./output; echo $? で exit code が期待値と一致するか確認
 2. Phase 3: gcc -c test.s -o test_ref.o && objdump -d test_ref.o と自作出力を比較
 3. Phase 4以降: ラベル付きのアセンブリをアセンブルし、readelf -a で構造を確認、実行して動作検証
 4. 最終目標: 自作コンパイラの出力をmasmでアセンブルし、正しく動作するバイナリが得られること
