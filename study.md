# masm 完全解説 — 自作アセンブラの仕組みを徹底的に理解する

## 目次

1. [masmとは何か](#1-masmとは何か)
2. [対応命令・オペランド一覧（リファレンス）](#2-対応命令オペランド一覧リファレンス)
3. [使い方（ビルドとテスト）](#3-使い方ビルドとテスト)
4. [全体の処理フロー](#4-全体の処理フロー)
5. [ステージ1: 入力の読み込み](#5-ステージ1-入力の読み込み)
6. [ステージ2: トークナイザ](#6-ステージ2-トークナイザ)
7. [ステージ3: パーサ（命令の解析）](#7-ステージ3-パーサ命令の解析)
8. [ステージ4: Pass 1 — サイズ計算とラベル解決](#8-ステージ4-pass-1--サイズ計算とラベル解決)
9. [ステージ5: Pass 2 — 機械語生成](#9-ステージ5-pass-2--機械語生成)
10. [ステージ6: ELFバイナリ出力](#10-ステージ6-elfバイナリ出力)
11. [x86-64 命令エンコーディング詳解](#11-x86-64-命令エンコーディング詳解)
12. [ELFフォーマット詳解](#12-elfフォーマット詳解)
13. [テストの仕組み](#13-テストの仕組み)
14. [具体例で追う: exit(42) ができるまで](#14-具体例で追う-exit42-ができるまで)
15. [具体例で追う: Hello World ができるまで](#15-具体例で追う-hello-world-ができるまで)

---

## 1. masmとは何か

masmは、x86-64アセンブリ（Intel構文）をLinux上で動作するELF実行可能バイナリに変換する自作アセンブラです。

普通のアセンブリ開発では、こういう流れです：

```
ソースコード(.c) → コンパイラ(gcc) → アセンブリ(.s) → アセンブラ(as/nasm) → オブジェクト(.o) → リンカ(ld) → 実行可能バイナリ(a.out)
```

masmは「アセンブラ」の部分を自作で置き換え、さらにリンカの仕事も一部含めて、直接ELF実行可能ファイルを出力します：

```
アセンブリ(stdin) → masm → 実行可能バイナリ(a.out)
                     ↑ ここを自作
```

---

## 2. 対応命令・オペランド一覧（リファレンス）

masmが現在サポートしている全命令と全オペランド形式の一覧です。
コードを読む前に「何ができるか」を把握するためのリファレンスとして使ってください。

### データ移動（MOV）

| 構文 | 説明 | エンコード例 |
|------|------|-------------|
| `mov r64, imm32` | レジスタに即値をセット | `48 C7 C0 2A000000` |
| `mov r64, r64` | レジスタ間コピー | `48 89 D8` |
| `mov r64, [r64+disp]` | メモリからロード | `48 8B 44 24 08` |
| `mov [r64+disp], r64` | メモリへストア | `48 89 04 24` |
| `mov r64, label` | ラベルアドレスをロード（movabs/64bit即値） | `48 B8` + imm64 |

### 算術・論理演算（ALU）

| 命令 | reg, reg | reg, imm32 | オペコード(reg) | ModR/M拡張(imm) |
|------|----------|------------|----------------|----------------|
| `add` | `add rax, rbx` | `add rax, 42` | `0x01` | `/0` |
| `sub` | `sub rax, rbx` | `sub rax, 42` | `0x29` | `/5` |
| `xor` | `xor rax, rbx` | `xor rax, 42` | `0x31` | `/6` |
| `and` | `and rax, rbx` | `and rax, 42` | `0x21` | `/4` |
| `or` | `or rax, rbx` | `or rax, 42` | `0x09` | `/1` |

### 比較・スタック・制御フロー

| 命令 | 説明 | バイナリ |
|------|------|---------|
| `cmp r64, r64/imm32` | 比較 | `0x39` / `0x81 /7` |
| `push r64` | スタックにプッシュ | `0x50+reg` |
| `pop r64` | スタックからポップ | `0x58+reg` |
| `ret` | リターン | `0xC3` |
| `syscall` | システムコール | `0x0F 0x05` |
| `jmp label` | 無条件ジャンプ | `0xE9` + rel32 |
| `call label` | サブルーチン呼び出し | `0xE8` + rel32 |

### 条件分岐（Jcc）

| 命令 | 条件 | エンコード |
|------|------|-----------|
| `je` / `jz` | 等しい | `0x0F 0x84` + rel32 |
| `jne` / `jnz` | 等しくない | `0x0F 0x85` + rel32 |
| `jl` | 小さい（符号付き） | `0x0F 0x8C` + rel32 |
| `jge` | 以上（符号付き） | `0x0F 0x8D` + rel32 |
| `jle` | 以下（符号付き） | `0x0F 0x8E` + rel32 |
| `jg` | 大きい（符号付き） | `0x0F 0x8F` + rel32 |

### 対応レジスタ（全16本の64bitレジスタ）

| 区分 | レジスタ | 番号(code) | REX拡張 |
|------|---------|-----------|---------|
| 基本 | `rax`, `rcx`, `rdx`, `rbx`, `rsp`, `rbp`, `rsi`, `rdi` | 0〜7 | 不要 |
| 拡張 | `r8`, `r9`, `r10`, `r11`, `r12`, `r13`, `r14`, `r15` | 0〜7 | REX.B必要 |

### 即値（イミディエイト）

- **32bit即値**: `mov rax, 42`、`add rbx, 0xFF` など
- **10進数**: `42`, `100`
- **16進数**: `0xFF`, `0x1000`（`0x` プレフィックス）
- **負の数**: `-1`, `-128`

### メモリアドレッシング

| 形式 | 例 | 備考 |
|------|-----|------|
| `[reg]` | `[rax]`, `[rbx]` | ベースレジスタのみ |
| `[reg + disp]` | `[rbp + 8]`, `[rsp + 16]` | ベース + 正ディスプレースメント |
| `[reg - disp]` | `[rbp - 8]`, `[rsp - 16]` | ベース + 負ディスプレースメント |

特殊処理:
- **rsp/r12** 使用時 → 自動的にSIBバイトを付加（ハードウェア仕様上の制約）
- **rbp/r13** 使用時 → mod=01 + disp8=0 の特殊エンコード（mod=00が[rip+disp32]と衝突するため）

### ラベル・定数・ディレクティブ

| 構文 | 説明 | 例 |
|------|------|-----|
| `label:` | ラベル定義 | `end:`, `loop_start:` |
| `name: equ VALUE` | コンパイル時定数 | `len: equ 14` |
| `section .text` | コードセクション開始 | |
| `section .data` | データセクション開始 | |
| `db` | バイト列/文字列を出力 | `db "Hello", 10` |
| `; comment` | コメント（行末まで無視） | `; これはコメント` |

---

## 3. 使い方（ビルドとテスト）

### ビルド

```bash
make          # gcc -Wall -Wextra -g -std=c11 -o masm main.c
```

これで `masm` という実行可能ファイルが生成されます。

### 手動で実行

```bash
echo "mov rax, 60
mov rdi, 42
syscall" | ./masm           # アセンブルして a.out を生成
chmod +x a.out              # 実行権限を付与
./a.out                     # 実行
echo $?                     # → 42
```

処理の流れ：
1. `echo` でアセンブリソースを標準入力に流す
2. `./masm` がそれを読んで `a.out` ファイルを生成
3. `chmod +x` で実行権限を付与（masmは自動で付けないため）
4. `./a.out` でバイナリを直接実行
5. `echo $?` で終了コード（exit code）を確認

### テスト実行

```bash
make test     # ビルド + 全テスト実行
# または
bash test.sh  # テストだけ実行（ビルド済みの場合）
```

---

## 4. 全体の処理フロー

masmの処理は大きく6つのステージに分かれます：

```
┌─────────────────────────────────────────────────────────┐
│  stdin (アセンブリソース文字列)                           │
│  例: "mov rax, 60\nmov rdi, 42\nsyscall\n"              │
└─────────────┬───────────────────────────────────────────┘
              │ ① read_all_stdin()
              ▼
┌─────────────────────────────────────────────────────────┐
│  char *src  (メモリ上の生テキスト)                        │
└─────────────┬───────────────────────────────────────────┘
              │ ② tokenize(src)
              ▼
┌─────────────────────────────────────────────────────────┐
│  Token 連結リスト                                        │
│  [IDENT"mov"] → [IDENT"rax"] → [COMMA] → [NUM 60]      │
│  → [NEWLINE] → [IDENT"mov"] → ...                       │
└─────────────┬───────────────────────────────────────────┘
              │ ③ parse(tokens)
              ▼
┌─────────────────────────────────────────────────────────┐
│  Insn 連結リスト（中間表現）                              │
│  [MOV_REG_IMM reg1=0 imm=60]                            │
│  → [MOV_REG_IMM reg1=7 imm=42]                          │
│  → [SYSCALL]                                             │
└─────────────┬───────────────────────────────────────────┘
              │ ④ pass1(insns) — サイズ計算 + ラベル登録
              ▼
┌─────────────────────────────────────────────────────────┐
│  各Insnに offset と size が設定される                     │
│  シンボルテーブルにラベルのアドレスが登録される             │
└─────────────┬───────────────────────────────────────────┘
              │ ⑤ pass2(insns) — 機械語生成
              ▼
┌─────────────────────────────────────────────────────────┐
│  code_buf[] (機械語バイト列)                              │
│  [48 c7 c0 3c 00 00 00]  ← mov rax, 60                 │
│  [48 c7 c7 2a 00 00 00]  ← mov rdi, 42                 │
│  [0f 05]                  ← syscall                      │
└─────────────┬───────────────────────────────────────────┘
              │ ⑥ write_elf("a.out")
              ▼
┌─────────────────────────────────────────────────────────┐
│  a.out (ELF実行可能バイナリファイル)                      │
│  [ELFヘッダ 64B] [プログラムヘッダ 56B] [機械語]          │
└─────────────────────────────────────────────────────────┘
```

対応するコード（`main()` 関数）：

```c
int main(void) {
    // ① 標準入力を全部読む
    char *src = read_all_stdin();

    // ② テキスト → トークン列
    Token *tokens = tokenize(src);

    // ③ トークン列 → 命令列（中間表現）
    Insn *insns = parse(tokens);

    // ④ Pass 1: 各命令のサイズとラベルのオフセットを計算
    pass1(insns);

    // ベースアドレス設定（ELFヘッダのサイズ分ずらす）
    int num_phdrs = (data_len > 0) ? 2 : 1;
    int headers_size = 64 + 56 * num_phdrs;
    text_base_addr = 0x400000 + headers_size;
    if (data_len > 0) {
        uint64_t data_offset = ((headers_size + code_len + 0xFFF) & ~0xFFFULL);
        data_base_addr = 0x400000 + data_offset;
    }

    // ⑤ Pass 2: 中間表現 → 機械語バイト列
    pass2(insns);

    // ⑥ ELFファイルとして書き出す
    write_elf("a.out");
    return 0;
}
```

---

## 5. ステージ1: 入力の読み込み

```c
static char *read_all_stdin(void) {
    size_t cap = 4096;    // バッファの初期容量
    size_t len = 0;       // 読み込み済みバイト数
    char *buf = malloc(cap);
    while (1) {
        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0) break;          // EOF
        if (len == cap) {
            cap *= 2;               // バッファが足りなくなったら倍に拡張
            buf = realloc(buf, cap);
        }
    }
    buf[len] = '\0';  // NUL終端
    return buf;
}
```

**仕組み:**
- 標準入力（stdin）から全データを一度にメモリに読み込みます
- バッファが足りなくなったら `realloc` で倍に拡張する「動的バッファ」パターン
- 最後にNUL終端して、C文字列として扱えるようにします

**なぜ一括読み込みか:**
- 後段のトークナイザが文字列全体をスキャンするので、全部メモリにある必要がある
- 行ごとに処理するとトークナイザの設計が複雑になる

---

## 6. ステージ2: トークナイザ

トークナイザは、生テキストを「トークン」という意味のある最小単位に分割します。

### トークンの種類

```c
typedef enum {
    TK_IDENT,      // 識別子（レジスタ名、命令名、ラベル名など）例: "mov", "rax", "end"
    TK_NUM,        // 数値リテラル  例: 42, 0xFF
    TK_STRING,     // 文字列リテラル  例: "Hello, world!"
    TK_COMMA,      // カンマ ','
    TK_COLON,      // コロン ':'
    TK_NEWLINE,    // 改行 '\n'
    TK_LBRACKET,   // 左角括弧 '['
    TK_RBRACKET,   // 右角括弧 ']'
    TK_PLUS,       // プラス '+'
    TK_MINUS,      // マイナス '-'
    TK_EOF,        // 入力終端
} TokenKind;
```

### トークンの構造

```c
struct Token {
    TokenKind kind;    // トークンの種類
    Token *next;       // 次のトークンへのポインタ（連結リスト）
    int64_t val;       // TK_NUM の場合の数値
    char *str;         // TK_IDENT, TK_STRING の場合の文字列
    int line;          // ソースの行番号（エラーメッセージ用）
};
```

### 処理の具体例

入力テキスト：
```
mov rax, 60  ; sys_exit
mov rdi, 42
syscall
```

トークナイズ結果（連結リスト）：
```
[IDENT "mov"] → [IDENT "rax"] → [COMMA] → [NUM 60] → [NEWLINE]
→ [IDENT "mov"] → [IDENT "rdi"] → [COMMA] → [NUM 42] → [NEWLINE]
→ [IDENT "syscall"] → [NEWLINE]
→ [EOF]
```

注意点：
- `; sys_exit` のコメントはスキップされて、トークン列には含まれない
- 空白・タブもスキップ
- `rax,` の `,` は独立したトークン（`TK_IDENT "rax"` + `TK_COMMA`）

### コードの流れ

```c
static Token *tokenize(const char *src) {
    Token head = {};         // ダミーの先頭ノード
    Token *cur = &head;      // 現在の末尾
    int line = 1;
    const char *p = src;     // 走査ポインタ

    while (*p) {
        // 1文字ずつ見て、何のトークンか判定
        if (*p == '\n')    → TK_NEWLINE を作成、line++
        if (空白)           → スキップ
        if (*p == ';')     → 行末までスキップ（コメント）
        if (*p == '"')     → 閉じ '"' まで読んで TK_STRING
        if (isdigit(*p))   → strtol() で数値を読んで TK_NUM
        if (isalpha(*p))   → 英数字が続く限り読んで TK_IDENT
        if (*p == ',')     → TK_COMMA
        // ... 他の記号も同様
    }

    cur->next = new_token(TK_EOF, line);  // 終端マーカー
    return head.next;  // ダミーの次＝実際の先頭トークン
}
```

**連結リスト構築のテクニック：「ダミーヘッド」パターン**

```c
Token head = {};         // ダミーノード（実データではない）
Token *cur = &head;

// ループ中:
cur->next = new_token(...);  // 新しいトークンを末尾に追加
cur = cur->next;             // 末尾ポインタを更新

return head.next;  // ダミーの「次」が実際の先頭
```

こうすると「リストが空の場合」と「リストに要素がある場合」を分けて処理する必要がなくなります。

---

## 7. ステージ3: パーサ（命令の解析）

パーサは、トークン列を消費しながら「中間表現（IR: Intermediate Representation）」に変換します。

### 中間表現の種類

```c
typedef enum {
    IN_MOV_REG_IMM,     // mov rax, 42       (レジスタ ← 即値)
    IN_MOV_REG_REG,     // mov rax, rbx      (レジスタ ← レジスタ)
    IN_MOV_REG_MEM,     // mov rax, [rbp-8]  (レジスタ ← メモリ)
    IN_MOV_MEM_REG,     // mov [rbp-8], rax  (メモリ ← レジスタ)
    IN_MOV_REG_LABEL,   // mov rsi, msg      (レジスタ ← ラベルアドレス)
    IN_ALU_REG_REG,     // add rax, rbx      (ALU演算 レジスタ間)
    IN_ALU_REG_IMM,     // add rax, 10       (ALU演算 レジスタ+即値)
    IN_CMP_REG_IMM,     // cmp rax, 5        (比較 レジスタ vs 即値)
    IN_CMP_REG_REG,     // cmp rax, rbx      (比較 レジスタ vs レジスタ)
    IN_PUSH,            // push rax
    IN_POP,             // pop rax
    IN_RET,             // ret
    IN_SYSCALL,         // syscall
    IN_JMP,             // jmp label
    IN_JCC,             // je label / jne label / ...
    IN_CALL,            // call label
    IN_LABEL,           // ラベル定義（命令ではないが位置管理に必要）
} InsnKind;
```

### パーサの動き方

パーサは「現在のトークン」を指すグローバルポインタ `tok` を進めながら処理します：

```c
static Token *tok;   // 現在のトークンへのポインタ

// トークンを1つ消費して次に進める例：
static Token *expect_ident(void) {
    if (tok->kind != TK_IDENT)
        error("line %d: expected identifier", tok->line);
    Token *t = tok;
    tok = tok->next;   // 次のトークンに進む
    return t;
}
```

### パーサのメインループ

```
while (!at_eof()) {
    ① "section .data" / "section .text" を処理
    ② "label:" をラベルとして登録
    ③ "db" でデータバイトを emit
    ④ ニーモニック(命令名)を読んで、それぞれの命令に分岐:
       "syscall" → IN_SYSCALL
       "ret"     → IN_RET
       "push"    → レジスタを読んで IN_PUSH
       "mov"     → オペランドの形式で分岐:
                    mov reg, imm   → IN_MOV_REG_IMM
                    mov reg, reg   → IN_MOV_REG_REG
                    mov reg, [mem] → IN_MOV_REG_MEM
                    mov [mem], reg → IN_MOV_MEM_REG
                    mov reg, label → IN_MOV_REG_LABEL
       ...
}
```

### レジスタの解決

レジスタ名は文字列なので、テーブルで番号に変換します：

```c
static const RegInfo regs[] = {
    {"rax", 0, 0}, {"rcx", 1, 0}, {"rdx", 2, 0}, {"rbx", 3, 0},
    {"rsp", 4, 0}, {"rbp", 5, 0}, {"rsi", 6, 0}, {"rdi", 7, 0},
    {"r8",  0, 1}, {"r9",  1, 1}, {"r10", 2, 1}, {"r11", 3, 1},
    {"r12", 4, 1}, {"r13", 5, 1}, {"r14", 6, 1}, {"r15", 7, 1},
};
```

各エントリの意味：
- `name`: レジスタ名（文字列）
- `code`: レジスタ番号（0〜7）。x86-64では3ビットでレジスタを指定する
- `rex_ext`: r8〜r15 の場合は 1。REXプレフィックスのビットで拡張が必要

**なぜ r8 の code が 0 なのか？**

x86-64のレジスタ番号は元々3ビット（0〜7）ですが、r8〜r15を追加するために
REXプレフィックスの1ビットを使って4ビットに拡張しました：

```
rax = 0000 (code=0, rex_ext=0)  →  r8  = 1000 (code=0, rex_ext=1)
rcx = 0001 (code=1, rex_ext=0)  →  r9  = 1001 (code=1, rex_ext=1)
rdx = 0010 (code=2, rex_ext=0)  →  r10 = 1010 (code=2, rex_ext=1)
...
rdi = 0111 (code=7, rex_ext=0)  →  r15 = 1111 (code=7, rex_ext=1)
```

---

## 8. ステージ4: Pass 1 — サイズ計算とラベル解決

### なぜ2パス必要か

こんなコードを考えてみましょう：

```asm
jmp end         ; ← end の位置はまだわからない！
mov rdi, 99
end:            ; ← ここが end の位置
mov rax, 60
```

`jmp end` をアセンブルするとき、`end` がどこにあるかわからなければ
ジャンプ先のオフセットが計算できません。

これが「前方参照問題」で、解決するために2パスアセンブルを行います：

```
Pass 1: 全命令のサイズを計算 → ラベルのオフセットを確定
Pass 2: ラベル参照を解決して機械語を生成
```

### Pass 1 の処理

```c
static void pass1(Insn *insns) {
    int offset = 0;  // 現在の命令のオフセット（コード先頭から何バイト目か）

    for (Insn *in = insns; in; in = in->next) {
        in->offset = offset;   // この命令の開始位置を記録

        switch (in->kind) {
        case IN_LABEL:
            in->size = 0;      // ラベルはバイトを消費しない
            // シンボルテーブルに (名前, オフセット) を登録
            add_symbol(in->label, offset, ...);
            break;

        case IN_MOV_REG_IMM:
            in->size = 7;      // REX(1) + opcode(1) + ModR/M(1) + imm32(4) = 7バイト
            break;

        case IN_SYSCALL:
            in->size = 2;      // 0F 05 = 2バイト
            break;

        case IN_JMP:
            in->size = 5;      // E9 + rel32 = 5バイト
            break;

        case IN_JCC:
            in->size = 6;      // 0F 8x + rel32 = 6バイト
            break;

        // ... 他の命令も同様
        }

        offset += in->size;  // 次の命令の開始位置
    }
}
```

### 具体例: ラベルオフセットの計算

```
命令               offset  size
─────────────────────────────────
jmp end            0       5      (E9 + rel32)
mov rdi, 99        5       7      (REX + C7 + ModR/M + imm32)
end:               12      0      ← ラベルは size=0、offset=12 で登録
mov rax, 60        12      7
xor rdi, rdi       19      3
syscall            22      2
```

これで `end` のオフセット(12) がわかったので、Pass 2 で `jmp end` のジャンプ先を計算できます。

### シンボルテーブル

```c
typedef struct {
    char name[128];    // ラベル名
    int offset;        // .text セクション先頭からのオフセット
    int is_data;       // .data セクションのラベルか？
    int data_offset;   // .data セクション内のオフセット
} Symbol;

static Symbol symbols[MAX_SYMBOLS];
static int num_symbols;
```

ラベルは2種類あります：
- **テキストラベル** (`end:`, `set_exit:`) → コードの中の位置。ジャンプ/コール先
- **データラベル** (`msg:`) → データセクションの中の位置。文字列などのアドレス

---

## 9. ステージ5: Pass 2 — 機械語生成

Pass 1 でラベルのアドレスが全部確定したので、実際のバイト列を生成します。

### 基本的な仕組み

グローバルバッファ `code_buf[]` にバイトを1つずつ追加していきます：

```c
static uint8_t code_buf[CODE_MAX];
static int code_len;

static void emit(uint8_t b) {
    code_buf[code_len++] = b;
}

static void emit_le32(uint32_t v) {
    emit(v & 0xFF);          // 最下位バイト
    emit((v >> 8) & 0xFF);
    emit((v >> 16) & 0xFF);
    emit((v >> 24) & 0xFF);  // 最上位バイト
}
```

`emit_le32` はリトルエンディアンで32ビット値を書き出します。
x86はリトルエンディアン（下位バイトが先）なので、値 `0x0000003C` (=60) は
メモリ上で `3C 00 00 00` になります。

### ジャンプ先アドレスの計算

`jmp end` のエンコードは `E9 <相対オフセット>` です。
相対オフセットの計算式：

```
rel32 = ジャンプ先オフセット - (現在の命令のオフセット + 命令サイズ)
```

なぜ「+ 命令サイズ」なのか？
→ CPUは命令を実行する前にプログラムカウンタ(PC/RIP)を次の命令に進めるため、
  ジャンプの基準は「jmp命令の次の命令のアドレス」になります。

```
例: jmp end (offset=0, size=5), end: (offset=12)
rel32 = 12 - (0 + 5) = 7
→ 機械語: E9 07 00 00 00
```

---

## 10. ステージ6: ELFバイナリ出力

最終的に、機械語バイト列をELFフォーマットでファイルに書き出します。

### 出力ファイルの構造

```
┌────────────────────────────────┐  オフセット 0
│  ELF ヘッダ (64バイト)          │
├────────────────────────────────┤  オフセット 64
│  プログラムヘッダ1 (56バイト)   │  ← .text セグメント情報
├────────────────────────────────┤  オフセット 120
│  [プログラムヘッダ2 (56バイト)] │  ← .data セグメント情報（あれば）
├────────────────────────────────┤  オフセット 120 or 176
│  .text (機械語)                │
├────────────────────────────────┤
│  [パディング (0埋め)]          │  ← ページ境界に揃える
├────────────────────────────────┤
│  [.data (文字列等)]            │  ← あれば
└────────────────────────────────┘
```

### 仮想アドレスの計算

Linuxのプロセスはファイルをそのままメモリにマップして実行します：

```
ファイル上の位置:               メモリ上のアドレス:
offset 0  (ELFヘッダ)      →  0x400000
offset 120 (機械語先頭)     →  0x400078  ← エントリポイント
                                          （ベース 0x400000 + ヘッダ 120バイト）
offset 0x1000 (.data)      →  0x401000  ← ページ境界揃え
```

**ベースアドレス `0x400000` とは？**
Linux x86-64の実行可能ファイルが伝統的にロードされるアドレスです。
カーネルのELFローダがプログラムヘッダの `p_vaddr` を見て、
ファイルの中身をこのアドレスにマッピングします。

---

## 11. x86-64 命令エンコーディング詳解

### 命令バイト列の一般的な構造

```
[REXプレフィックス] [オペコード (1〜2バイト)] [ModR/M] [SIB] [ディスプレースメント] [即値]
   0 or 1バイト      必須                      0 or 1  0 or 1  0,1,4バイト       0,1,4バイト
```

### REX プレフィックス (1バイト)

64ビットモードで使われる拡張プレフィックス：

```
ビット: 0100 W R X B

W (bit 3): 1 = 64ビットオペランドサイズ
R (bit 2): ModR/M の reg フィールドを拡張 (r8-r15用)
X (bit 1): SIB の index フィールドを拡張
B (bit 0): ModR/M の r/m フィールド or SIB の base を拡張 (r8-r15用)
```

例：
- `0x48` = `0100 1000` → REX.W (64ビットオペランド、拡張なし)
- `0x49` = `0100 1001` → REX.W + REX.B (64ビット + r/m拡張)
- `0x4C` = `0100 1100` → REX.W + REX.R (64ビット + reg拡張)

### ModR/M バイト (1バイト)

命令のオペランドの種類とレジスタ番号を指定します：

```
ビット: [mod (2)] [reg (3)] [r/m (3)]

mod: オペランドの種類
  11 = レジスタ直接  (例: mov rax, rbx)
  00 = メモリ [r/m]  (例: mov rax, [rbx])
  01 = メモリ [r/m + disp8]   (例: mov rax, [rbx + 4])
  10 = メモリ [r/m + disp32]  (例: mov rax, [rbx + 1000])

reg: レジスタ番号 or 命令の拡張オペコード (/0〜/7)
r/m: レジスタ番号 or メモリのベースレジスタ
```

### 各命令のエンコーディング

#### `mov r64, imm32` — レジスタに即値を代入

```
バイト列: [REX.W] [C7] [ModR/M: 11 000 reg] [imm32 リトルエンディアン]

例: mov rax, 60
  REX.W = 0x48
  C7 = opcode
  ModR/M = 11 000 000 = 0xC0  (mod=11=レジスタ, /0, r/m=0=rax)
  imm32 = 60 = 0x3C 0x00 0x00 0x00
  → 48 C7 C0 3C 00 00 00

例: mov rdi, 42
  REX.W = 0x48
  ModR/M = 11 000 111 = 0xC7  (r/m=7=rdi)
  → 48 C7 C7 2A 00 00 00
```

#### `syscall` — システムコール

```
固定2バイト: 0F 05
```

#### `mov r64, r64` — レジスタ間コピー

```
バイト列: [REX.W] [89] [ModR/M: 11 src dst]

例: mov rax, rbx
  REX.W = 0x48
  89 = opcode (MOV r/m64, r64)
  ModR/M = 11 011 000 = 0xD8  (mod=11, reg=3=rbx, r/m=0=rax)
  → 48 89 D8
```

#### ALU演算 (`add`, `sub`, `xor`, `and`, `or`)

**レジスタ間：**
```
バイト列: [REX.W] [opcode] [ModR/M: 11 src dst]

各命令のオペコード:
  add → 0x01
  sub → 0x29
  xor → 0x31
  and → 0x21
  or  → 0x09

例: xor rdi, rdi
  REX.W = 0x48, opcode = 0x31
  ModR/M = 11 111 111 = 0xFF  (reg=7=rdi, r/m=7=rdi)
  → 48 31 FF
```

**レジスタ + 即値：**
```
バイト列: [REX.W] [81] [ModR/M: 11 /r reg] [imm32]

/r は命令ごとに違う:
  add → /0,  sub → /5,  xor → /6,  and → /4,  or → /1

例: sub rsp, 8
  REX.W = 0x48, 81, ModR/M = 11 101 100 = 0xEC  (/5=sub, r/m=4=rsp)
  imm32 = 08 00 00 00
  → 48 81 EC 08 00 00 00
```

#### `push r64` / `pop r64`

```
push: 50 + レジスタ番号  (1バイト)
pop:  58 + レジスタ番号  (1バイト)

例: push rdi → 50 + 7 = 0x57
例: pop rdi  → 58 + 7 = 0x5F

r8-r15 の場合は REX.B プレフィックス(0x41)が先に付く:
例: push r8 → 41 50
```

#### `ret`

```
固定1バイト: C3
```

#### `jmp rel32` — 無条件ジャンプ

```
バイト列: [E9] [rel32]  (5バイト)
rel32 = ジャンプ先 - (jmp命令のアドレス + 5)
```

#### `jcc rel32` — 条件ジャンプ

```
バイト列: [0F] [cc] [rel32]  (6バイト)
rel32 = ジャンプ先 - (jcc命令のアドレス + 6)

cc の値:
  je  = 0x84    jne = 0x85
  jl  = 0x8C    jge = 0x8D
  jle = 0x8E    jg  = 0x8F
```

#### `call rel32` — サブルーチン呼び出し

```
バイト列: [E8] [rel32]  (5バイト)
rel32 = 呼び出し先 - (call命令のアドレス + 5)

call は戻りアドレス(call命令の次のアドレス)をスタックに push してからジャンプ。
ret はスタックから pop してそのアドレスにジャンプ。
```

#### `cmp r64, imm32` — 比較

```
バイト列: [REX.W] [81] [ModR/M: 11 111 reg] [imm32]
                              (/7 = cmp)
```

#### メモリオペランド `[reg]`, `[reg + disp]`

ModR/M の mod フィールドで指定：

```
mod=00: [reg]           ディスプレースメントなし
mod=01: [reg + disp8]   8ビット符号付きオフセット (-128〜+127)
mod=10: [reg + disp32]  32ビット符号付きオフセット
```

**特殊ケース1: `[rsp]` は SIB バイト必須**

rsp のレジスタ番号(4) は ModR/M で「SIBバイトが続く」というエスケープに使われるため、
rsp をベースにしたメモリアクセスには必ず SIB バイトが必要です：

```
SIBバイト: [scale(2)] [index(3)] [base(3)]

[rsp] の場合: SIB = 00 100 100 = 0x24
              (scale=1, index=rsp=なし, base=rsp)
```

**特殊ケース2: `[rbp]` は mod=00 が使えない**

rbp のレジスタ番号(5) + mod=00 は「[rip + disp32]」(PC相対)を意味するため、
`[rbp]` には mod=01 + disp8=0 を使います：

```
[rbp] → ModR/M = 01 reg 101, disp8 = 0x00
```

#### `movabs r64, imm64` — 64ビット即値ロード

データラベルのアドレスは64ビットなので、特殊な形式を使います：

```
バイト列: [REX.W] [B8 + reg] [imm64 (8バイト)]  合計10バイト

例: mov rsi, msg (msgのアドレスが 0x0000000000401000 の場合)
  → 48 BE 00 10 40 00 00 00 00 00
```

---

## 12. ELFフォーマット詳解

### ELFヘッダ (64バイト)

```
オフセット  サイズ  フィールド          値               説明
────────────────────────────────────────────────────────────
0x00       16     e_ident             7F 45 4C 46 ...  マジックナンバー等
0x10       2      e_type              0x0002           ET_EXEC (実行可能)
0x12       2      e_machine           0x003E           EM_X86_64
0x14       4      e_version           0x00000001       ELFバージョン
0x18       8      e_entry             0x0040????       エントリポイント
0x20       8      e_phoff             0x00000040 (=64) プログラムヘッダのファイル位置
0x28       8      e_shoff             0                セクションヘッダなし
0x30       4      e_flags             0                フラグなし
0x34       2      e_ehsize            0x0040 (=64)     ELFヘッダサイズ
0x36       2      e_phentsize         0x0038 (=56)     プログラムヘッダ1つのサイズ
0x38       2      e_phnum             1 or 2           プログラムヘッダの数
0x3A       2      e_shentsize         0                セクションヘッダなし
0x3C       2      e_shnum             0                セクションヘッダなし
0x3E       2      e_shstrndx          0
```

`e_ident` の内訳：
```
0x7F 'E' 'L' 'F'   マジックナンバー (全ELFファイルの先頭)
0x02                ELFCLASS64 (64ビット)
0x01                ELFDATA2LSB (リトルエンディアン)
0x01                EV_CURRENT (ELFバージョン)
0x00                ELFOSABI_NONE (OS非依存)
0x00 x 8            パディング
```

### プログラムヘッダ (各56バイト)

プログラムヘッダは「このファイルのどの部分を、メモリのどこに、どんな権限で読み込むか」を指定します。

**ヘッダ1: .text (実行可能コード)**
```
オフセット  サイズ  フィールド    値            説明
──────────────────────────────────────────────────
0x00       4      p_type       1 (PT_LOAD)   メモリにロードするセグメント
0x04       4      p_flags      5 (R|X)       読み取り+実行権限
0x08       8      p_offset     0             ファイル先頭からロード
0x10       8      p_vaddr      0x400000      仮想アドレス
0x18       8      p_paddr      0x400000      物理アドレス（通常 vaddr と同じ）
0x20       8      p_filesz     (計算値)       ファイル上のサイズ
0x28       8      p_memsz      (計算値)       メモリ上のサイズ
0x30       8      p_align      0x1000        アライメント（4KBページ境界）
```

**ヘッダ2: .data (読み書きデータ)** ← .dataセクションがある場合のみ
```
p_type    = 1 (PT_LOAD)
p_flags   = 6 (R|W)        ← 読み取り+書き込み（実行不可）
p_offset  = 0x1000          ← ページ境界にアラインされたファイルオフセット
p_vaddr   = 0x401000        ← base + p_offset
```

### なぜページ境界に揃えるのか

Linuxのメモリ管理はページ(4KB = 0x1000バイト)単位で行われます。
各ページに別々の権限（読み取り/書き込み/実行）を設定するため、
.text（実行可能）と .data（書き込み可能）は別のページに配置する必要があります。

```
ページ 0x400000-0x400FFF: .text (R-X) 読み+実行
ページ 0x401000-0x401FFF: .data (RW-) 読み+書き
```

---

## 13. テストの仕組み

### test.sh の構造

テストスクリプトには2種類のアサーション関数があります：

**`assert_exit` — 終了コードを検証**

```bash
assert_exit 42 "exit(42)" \
"mov rax, 60
mov rdi, 42
syscall"
```

動作：
1. アセンブリソースを `echo` で `./masm` の stdin に流す
2. 生成された `a.out` に実行権限を付与
3. `a.out` を実行
4. 終了コード (`$?`) が期待値(42)と一致するか確認

**`assert_stdout` — 標準出力を検証**

```bash
assert_stdout "Hello, world!" "Hello world" \
'section .data
msg: db "Hello, world!", 10
...'
```

動作：
1. 同様にアセンブルして実行
2. `$()` で標準出力をキャプチャ
3. 出力文字列が期待値と一致するか確認

### テストケース一覧

| ステップ | テスト内容 | 検証方法 |
|---------|-----------|---------|
| Step 2 | mov rax, 60 + syscall | exit=0 |
| Step 3 | exit(42), exit(0) | exit=42, 0 |
| Step 5 | xor, add, sub | exit=0, 3, 5 |
| Step 6 | push/pop | exit=42 |
| Step 8 | jmp でラベルへジャンプ | exit=0 |
| Step 9 | cmp + jg/jle/je | exit=1, 0, 1 |
| Step 10 | call + ret | exit=42 |
| Step 11 | mov [rsp], mov [rsp] | exit=42 |
| Step 12 | .data + write(1, msg, 14) | stdout="Hello, world!" |

---

## 14. 具体例で追う: exit(42) ができるまで

入力：
```asm
mov rax, 60
mov rdi, 42
syscall
```

### ステップ1: トークナイズ

```
[IDENT "mov"] → [IDENT "rax"] → [COMMA] → [NUM 60] → [NEWLINE]
→ [IDENT "mov"] → [IDENT "rdi"] → [COMMA] → [NUM 42] → [NEWLINE]
→ [IDENT "syscall"] → [NEWLINE]
→ [EOF]
```

### ステップ2: パース

```
Insn 1: IN_MOV_REG_IMM  { reg1=0(rax), imm=60 }
Insn 2: IN_MOV_REG_IMM  { reg1=7(rdi), imm=42 }
Insn 3: IN_SYSCALL      {}
```

### ステップ3: Pass 1 (サイズ計算)

```
Insn 1: offset=0,  size=7   (REX+C7+ModRM+imm32)
Insn 2: offset=7,  size=7
Insn 3: offset=14, size=2   (0F 05)
code_len = 16バイト
```

### ステップ4: ベースアドレス計算

```
ヘッダサイズ = 64(ELF) + 56(PH) = 120バイト   (.dataなしなのでPH1つ)
text_base_addr = 0x400000 + 120 = 0x400078
entry = 0x400078
```

### ステップ5: Pass 2 (機械語生成)

```
Insn 1: mov rax, 60
  REX.W=0x48, opcode=0xC7, ModR/M=0xC0|(0)=0xC0, imm32=60
  → 48 C7 C0 3C 00 00 00

Insn 2: mov rdi, 42
  REX.W=0x48, opcode=0xC7, ModR/M=0xC0|(7)=0xC7, imm32=42
  → 48 C7 C7 2A 00 00 00

Insn 3: syscall
  → 0F 05

code_buf = [48 C7 C0 3C 00 00 00  48 C7 C7 2A 00 00 00  0F 05]
            ├── mov rax, 60 ──┤  ├── mov rdi, 42 ──┤  ├syscall┤
```

### ステップ6: ELF出力

```
a.out の中身:

オフセット   内容
──────────────────────────────────────
0x00-0x3F   ELFヘッダ (64バイト)
            e_entry = 0x400078
0x40-0x77   プログラムヘッダ (56バイト)
            p_vaddr = 0x400000
            p_filesz = 136 (120+16)
            p_memsz = 136
0x78-0x87   機械語 (16バイト)
            48 C7 C0 3C 00 00 00
            48 C7 C7 2A 00 00 00
            0F 05
```

### 実行時の動き

1. カーネルが a.out の ELFヘッダを読む
2. プログラムヘッダに従い、ファイルを仮想アドレス 0x400000 にマッピング
3. エントリポイント 0x400078 にジャンプ
4. `mov rax, 60` → rax = 60 (sys_exit のシステムコール番号)
5. `mov rdi, 42` → rdi = 42 (exit code)
6. `syscall` → カーネルに制御が移り、exit(42) が実行される
7. プロセス終了、親プロセスが `$?` で 42 を受け取る

---

## 15. 具体例で追う: Hello World ができるまで

入力：
```asm
section .data
msg: db "Hello, world!", 10
len: equ 14
section .text
mov rax, 1
mov rdi, 1
mov rsi, msg
mov rdx, len
syscall
mov rax, 60
xor rdi, rdi
syscall
```

### ステップ1: トークナイズ

```
[IDENT "section"] → [IDENT ".data"] → [NEWLINE]
→ [IDENT "msg"] → [COLON] → [IDENT "db"] → [STRING "Hello, world!"] → [COMMA] → [NUM 10] → [NEWLINE]
→ [IDENT "len"] → [COLON] → [IDENT "equ"] → [NUM 14] → [NEWLINE]
→ [IDENT "section"] → [IDENT ".text"] → [NEWLINE]
→ [IDENT "mov"] → [IDENT "rax"] → [COMMA] → [NUM 1] → [NEWLINE]
→ ...
→ [EOF]
```

### ステップ2: パース

パース中に以下が起きます：

1. `section .data` → `in_data_section = 1` に切り替え
2. `msg:` → データラベル。`data_len=0` の時点なので `mem_disp=0` を記録
3. `db "Hello, world!", 10` → `data_buf` に14バイトを書き込み (`data_len=14`)
4. `len: equ 14` → 定数テーブルに `len=14` を登録
5. `section .text` → `in_data_section = 0` に切り替え
6. 命令群をパース:
   - `mov rsi, msg` → `msg` はレジスタでも定数でもないのでラベル参照 → `IN_MOV_REG_LABEL`
   - `mov rdx, len` → `len` は定数(=14) → `IN_MOV_REG_IMM { imm=14 }`

パース結果：
```
[LABEL "msg" (is_data=1, data_offset=0)]
→ [MOV_REG_IMM  reg1=0(rax), imm=1]        ; mov rax, 1 (sys_write)
→ [MOV_REG_IMM  reg1=7(rdi), imm=1]        ; mov rdi, 1 (stdout)
→ [MOV_REG_LABEL reg1=6(rsi), label="msg"]  ; mov rsi, msg
→ [MOV_REG_IMM  reg1=2(rdx), imm=14]       ; mov rdx, 14 (len は定数展開済み)
→ [SYSCALL]
→ [MOV_REG_IMM  reg1=0(rax), imm=60]       ; mov rax, 60 (sys_exit)
→ [ALU_REG_REG  xor rdi, rdi]              ; xor rdi, rdi
→ [SYSCALL]
```

### ステップ3: Pass 1

```
Insn         offset  size
──────────────────────────
LABEL "msg"  0       0     → add_symbol("msg", 0, is_data=1, data_offset=0)
MOV_REG_IMM  0       7
MOV_REG_IMM  7       7
MOV_REG_LABEL 14     10    ← movabs (64bit即値) なので10バイト
MOV_REG_IMM  24      7
SYSCALL      31      2
MOV_REG_IMM  33      7
ALU_REG_REG  40      3
SYSCALL      43      2
```

### ステップ4: ベースアドレス計算

```
data_len=14 > 0 なので、プログラムヘッダは2つ

headers_size = 64 + 56*2 = 176 バイト
text_base_addr = 0x400000 + 176 = 0x4000B0
code_len = 45

data_offset = (176 + 45 + 0xFFF) & ~0xFFF = 0x1000
data_base_addr = 0x400000 + 0x1000 = 0x401000
```

### ステップ5: Pass 2

`mov rsi, msg` の機械語生成：
```
msgのアドレス = data_base_addr + data_offset_of_msg = 0x401000 + 0 = 0x401000

movabs rsi, 0x401000:
  REX.W = 0x48
  B8 + 6(rsi) = 0xBE
  imm64 = 00 10 40 00 00 00 00 00 (リトルエンディアン)
  → 48 BE 00 10 40 00 00 00 00 00
```

### ステップ6: ELF出力

```
a.out の中身:

0x0000-0x003F  ELFヘッダ (e_entry=0x4000B0, e_phnum=2)
0x0040-0x0077  プログラムヘッダ1 (.text: vaddr=0x400000, flags=R|X)
0x0078-0x00AF  プログラムヘッダ2 (.data: vaddr=0x401000, flags=R|W, offset=0x1000)
0x00B0-0x00DC  .text 機械語 (45バイト)
0x00DD-0x0FFF  0x00 パディング
0x1000-0x100D  .data: "Hello, world!\n" (14バイト)
```

### 実行時の動き

1. カーネルが2つのセグメントをマップ:
   - 0x400000: .text (読み+実行)
   - 0x401000: .data (読み+書き)
2. エントリポイント 0x4000B0 から実行開始
3. `mov rax, 1` → rax=1 (sys_write)
4. `mov rdi, 1` → rdi=1 (fd=stdout)
5. `mov rsi, 0x401000` → rsi = msgの仮想アドレス
6. `mov rdx, 14` → rdx=14 (書き込むバイト数)
7. `syscall` → write(1, "Hello, world!\n", 14) が実行される → 画面に出力
8. `mov rax, 60` → rax=60 (sys_exit)
9. `xor rdi, rdi` → rdi=0
10. `syscall` → exit(0)
