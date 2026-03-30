#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

// ============================================================
// エラーハンドリング
// ============================================================

static void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "masm: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// ============================================================
// 機械語バッファ
// ============================================================

#define CODE_MAX (64 * 1024)
#define DATA_MAX (64 * 1024)

static uint8_t code_buf[CODE_MAX];
static int code_len;

static uint8_t data_buf[DATA_MAX];
static int data_len;

static void emit(uint8_t b) {
    if (code_len >= CODE_MAX) error("code buffer overflow");
    code_buf[code_len++] = b;
}

static void emit_le32(uint32_t v) {
    emit(v & 0xFF);
    emit((v >> 8) & 0xFF);
    emit((v >> 16) & 0xFF);
    emit((v >> 24) & 0xFF);
}

static void emit_data(uint8_t b) {
    if (data_len >= DATA_MAX) error("data buffer overflow");
    data_buf[data_len++] = b;
}

// ============================================================
// トークナイザ (Step 4)
// ============================================================

typedef enum {
    TK_IDENT,
    TK_NUM,
    TK_STRING,
    TK_COMMA,
    TK_COLON,
    TK_NEWLINE,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_PLUS,
    TK_MINUS,
    TK_EOF,
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    Token *next;
    int64_t val;        // TK_NUM の場合
    char *str;          // TK_IDENT, TK_STRING の場合
    int line;
};

static Token *new_token(TokenKind kind, int line) {
    Token *t = calloc(1, sizeof(Token));
    t->kind = kind;
    t->line = line;
    return t;
}

static Token *tokenize(const char *src) {
    Token head = {};
    Token *cur = &head;
    int line = 1;
    const char *p = src;

    while (*p) {
        // 改行
        if (*p == '\n') {
            cur->next = new_token(TK_NEWLINE, line);
            cur = cur->next;
            line++;
            p++;
            continue;
        }

        // 空白・タブ
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }

        // コメント
        if (*p == ';') {
            while (*p && *p != '\n') p++;
            continue;
        }

        // 文字列リテラル
        if (*p == '"') {
            p++;
            const char *start = p;
            while (*p && *p != '"' && *p != '\n') p++;
            if (*p != '"') error("line %d: unterminated string", line);
            int len = p - start;
            cur->next = new_token(TK_STRING, line);
            cur = cur->next;
            cur->str = strndup(start, len);
            p++; // skip closing "
            continue;
        }

        // 数値
        if (isdigit(*p)) {
            cur->next = new_token(TK_NUM, line);
            cur = cur->next;
            char *end;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                cur->val = strtol(p, &end, 16);
            } else {
                cur->val = strtol(p, &end, 10);
            }
            p = end;
            continue;
        }

        // 負の数値（-の直後に数字）は TK_MINUS + TK_NUM で処理

        // 識別子（レジスタ名、命令名、ラベル名）
        // ドットから始まるディレクティブ名も含む
        if (isalpha(*p) || *p == '_' || *p == '.') {
            const char *start = p;
            while (isalnum(*p) || *p == '_' || *p == '.') p++;
            int len = p - start;
            cur->next = new_token(TK_IDENT, line);
            cur = cur->next;
            cur->str = strndup(start, len);
            continue;
        }

        // 記号
        if (*p == ',') { cur->next = new_token(TK_COMMA, line); cur = cur->next; p++; continue; }
        if (*p == ':') { cur->next = new_token(TK_COLON, line); cur = cur->next; p++; continue; }
        if (*p == '[') { cur->next = new_token(TK_LBRACKET, line); cur = cur->next; p++; continue; }
        if (*p == ']') { cur->next = new_token(TK_RBRACKET, line); cur = cur->next; p++; continue; }
        if (*p == '+') { cur->next = new_token(TK_PLUS, line); cur = cur->next; p++; continue; }
        if (*p == '-') { cur->next = new_token(TK_MINUS, line); cur = cur->next; p++; continue; }

        error("line %d: unexpected character '%c'", line, *p);
    }

    cur->next = new_token(TK_EOF, line);
    return head.next;
}

// ============================================================
// レジスタテーブル
// ============================================================

typedef struct {
    const char *name;
    int code;     // レジスタ番号 (0-7 for rax-rdi, 0-7 for r8-r15)
    int rex_ext;  // REX.B が必要か (r8-r15)
} RegInfo;

static const RegInfo regs[] = {
    {"rax", 0, 0}, {"rcx", 1, 0}, {"rdx", 2, 0}, {"rbx", 3, 0},
    {"rsp", 4, 0}, {"rbp", 5, 0}, {"rsi", 6, 0}, {"rdi", 7, 0},
    {"r8",  0, 1}, {"r9",  1, 1}, {"r10", 2, 1}, {"r11", 3, 1},
    {"r12", 4, 1}, {"r13", 5, 1}, {"r14", 6, 1}, {"r15", 7, 1},
};

static const RegInfo *find_reg(const char *name) {
    for (int i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
        if (strcmp(regs[i].name, name) == 0) return &regs[i];
    }
    return NULL;
}

// ============================================================
// シンボルテーブル (Step 7)
// ============================================================

#define MAX_SYMBOLS 1024

typedef struct {
    char name[128];
    int offset;        // コード先頭からのオフセット（.textラベル）
    int is_data;       // .data セクション内ラベルか
    int data_offset;   // .data セクション内のオフセット
} Symbol;

static Symbol symbols[MAX_SYMBOLS];
static int num_symbols;

// equ 定数テーブル
typedef struct {
    char name[128];
    int64_t value;
} Constant;

static Constant constants[MAX_SYMBOLS];
static int num_constants;

static void add_symbol(const char *name, int offset, int is_data, int data_offset) {
    if (num_symbols >= MAX_SYMBOLS) error("too many symbols");
    strncpy(symbols[num_symbols].name, name, 127);
    symbols[num_symbols].offset = offset;
    symbols[num_symbols].is_data = is_data;
    symbols[num_symbols].data_offset = data_offset;
    num_symbols++;
}

static Symbol *find_symbol(const char *name) {
    for (int i = 0; i < num_symbols; i++) {
        if (strcmp(symbols[i].name, name) == 0) return &symbols[i];
    }
    return NULL;
}

static void add_constant(const char *name, int64_t value) {
    if (num_constants >= MAX_SYMBOLS) error("too many constants");
    strncpy(constants[num_constants].name, name, 127);
    constants[num_constants].value = value;
    num_constants++;
}

static int64_t *find_constant(const char *name) {
    for (int i = 0; i < num_constants; i++) {
        if (strcmp(constants[i].name, name) == 0) return &constants[i].value;
    }
    return NULL;
}

// ============================================================
// 命令の中間表現 (Step 8: 2パスアセンブル)
// ============================================================

typedef enum {
    IN_MOV_REG_IMM,     // mov r64, imm32
    IN_MOV_REG_REG,     // mov r64, r64
    IN_MOV_REG_MEM,     // mov r64, [r64 + disp]
    IN_MOV_MEM_REG,     // mov [r64 + disp], r64
    IN_MOV_REG_LABEL,   // mov r64, label (data label の絶対アドレス)
    IN_ALU_REG_REG,     // add/sub/xor/and/or r64, r64
    IN_ALU_REG_IMM,     // add/sub/xor/and/or r64, imm32
    IN_CMP_REG_IMM,     // cmp r64, imm32
    IN_CMP_REG_REG,     // cmp r64, r64
    IN_PUSH,            // push r64
    IN_POP,             // pop r64
    IN_RET,             // ret
    IN_SYSCALL,         // syscall
    IN_JMP,             // jmp label
    IN_JCC,             // je/jne/jl/jge/jle/jg label
    IN_CALL,            // call label
    IN_LABEL,           // ラベル（命令ではないがオフセット管理用）
} InsnKind;

typedef struct Insn Insn;
struct Insn {
    InsnKind kind;
    int reg1;           // 宛先レジスタコード
    int reg1_ext;       // REX.B for reg1
    int reg2;           // ソースレジスタコード
    int reg2_ext;       // REX.B/REX.R for reg2
    int64_t imm;        // 即値
    char label[128];    // ジャンプ先ラベル名
    int alu_opcode;     // ALU命令のオペコード
    int jcc_opcode;     // 条件ジャンプのcc部分 (0x84=je, etc.)
    int offset;         // このinstのコード先頭からのオフセット（pass1で計算）
    int size;           // この命令のバイト数
    // メモリオペランド
    int mem_base;       // ベースレジスタコード
    int mem_base_ext;   // REX.B for base
    int32_t mem_disp;   // ディスプレースメント
    int has_disp;       // ディスプレースメントがあるか
    Insn *next;
};

static Insn *new_insn(InsnKind kind) {
    Insn *in = calloc(1, sizeof(Insn));
    in->kind = kind;
    return in;
}

// ============================================================
// パーサ
// ============================================================

static Token *tok;   // 現在のトークン

static int at_eof(void) { return tok->kind == TK_EOF; }

static Token *expect_ident(void) {
    if (tok->kind != TK_IDENT)
        error("line %d: expected identifier, got token kind %d", tok->line, tok->kind);
    Token *t = tok;
    tok = tok->next;
    return t;
}

static int64_t expect_number(void) {
    // 負の数の処理
    int neg = 0;
    if (tok->kind == TK_MINUS) {
        neg = 1;
        tok = tok->next;
    }
    if (tok->kind != TK_NUM)
        error("line %d: expected number", tok->line);
    int64_t v = tok->val;
    tok = tok->next;
    return neg ? -v : v;
}

static void expect(TokenKind kind) {
    if (tok->kind != kind)
        error("line %d: unexpected token (expected kind %d, got %d)", tok->line, kind, tok->kind);
    tok = tok->next;
}

static void skip_newlines(void) {
    while (tok->kind == TK_NEWLINE) tok = tok->next;
}

// ALU命令テーブル
typedef struct {
    const char *name;
    int opcode;  // r/m64, r64 形式のオペコード
    int imm_ext; // /r (ModR/M の reg フィールド) for r/m64, imm32
} AluInfo;

static const AluInfo alu_ops[] = {
    {"add", 0x01, 0},
    {"sub", 0x29, 5},
    {"xor", 0x31, 6},
    {"and", 0x21, 4},
    {"or",  0x09, 1},
};

static const AluInfo *find_alu(const char *name) {
    for (int i = 0; i < (int)(sizeof(alu_ops) / sizeof(alu_ops[0])); i++) {
        if (strcmp(alu_ops[i].name, name) == 0) return &alu_ops[i];
    }
    return NULL;
}

// 条件ジャンプテーブル
typedef struct {
    const char *name;
    int cc;  // 0F 8x の下位バイト
} JccInfo;

static const JccInfo jcc_ops[] = {
    {"je",  0x84}, {"jne", 0x85},
    {"jl",  0x8C}, {"jge", 0x8D},
    {"jle", 0x8E}, {"jg",  0x8F},
    {"jz",  0x84}, {"jnz", 0x85},
};

static const JccInfo *find_jcc(const char *name) {
    for (int i = 0; i < (int)(sizeof(jcc_ops) / sizeof(jcc_ops[0])); i++) {
        if (strcmp(jcc_ops[i].name, name) == 0) return &jcc_ops[i];
    }
    return NULL;
}

// メモリオペランドのパース: [reg], [reg + disp], [reg - disp]
// tok は '[' の次を指している状態で呼ぶ
static void parse_mem_operand(int *base, int *base_ext, int32_t *disp, int *has_disp) {
    Token *reg_tok = expect_ident();
    const RegInfo *ri = find_reg(reg_tok->str);
    if (!ri) error("line %d: unknown register '%s'", reg_tok->line, reg_tok->str);
    *base = ri->code;
    *base_ext = ri->rex_ext;
    *disp = 0;
    *has_disp = 0;

    if (tok->kind == TK_PLUS) {
        tok = tok->next;
        *disp = (int32_t)expect_number();
        *has_disp = 1;
    } else if (tok->kind == TK_MINUS) {
        tok = tok->next;
        *disp = -(int32_t)expect_number();
        *has_disp = 1;
    }

    expect(TK_RBRACKET);
}

// 即値またはシンボル参照を解決（定数名 or 数値リテラル）
// パース時点ではラベル参照は値がわからないので、名前を保持する
static int64_t parse_imm_or_const(char *label_out) {
    if (tok->kind == TK_IDENT) {
        // 定数参照かラベル参照
        Token *t = tok;
        tok = tok->next;
        int64_t *cv = find_constant(t->str);
        if (cv) {
            label_out[0] = '\0';
            return *cv;
        }
        // ラベル参照として扱う
        strncpy(label_out, t->str, 127);
        return 0;
    }
    label_out[0] = '\0';
    return expect_number();
}

// 現在のセクション
static int in_data_section;

static Insn *parse(Token *tokens) {
    tok = tokens;
    Insn head = {};
    Insn *cur = &head;
    in_data_section = 0;

    skip_newlines();

    while (!at_eof()) {
        // セクションディレクティブ
        if (tok->kind == TK_IDENT && strcmp(tok->str, "section") == 0) {
            tok = tok->next;
            Token *sec = expect_ident();
            if (strcmp(sec->str, ".text") == 0) {
                in_data_section = 0;
            } else if (strcmp(sec->str, ".data") == 0) {
                in_data_section = 1;
            } else {
                error("line %d: unknown section '%s'", sec->line, sec->str);
            }
            skip_newlines();
            continue;
        }

        // ラベル: IDENT の次が COLON ならラベル
        if (tok->kind == TK_IDENT && tok->next && tok->next->kind == TK_COLON) {
            Token *name_tok = tok;
            tok = tok->next; // IDENT
            tok = tok->next; // COLON

            if (in_data_section) {
                // データラベル - data_len の現在値がこのラベルのオフセット
                Insn *in = new_insn(IN_LABEL);
                strncpy(in->label, name_tok->str, 127);
                in->imm = 1; // is_data フラグ
                in->mem_disp = data_len; // パース時点の data_len を記録
                cur->next = in;
                cur = in;
            } else {
                Insn *in = new_insn(IN_LABEL);
                strncpy(in->label, name_tok->str, 127);
                in->imm = 0; // is_text フラグ
                cur->next = in;
                cur = in;
            }

            // equ ディレクティブ: "label: equ VALUE"
            if (tok->kind == TK_IDENT && strcmp(tok->str, "equ") == 0) {
                tok = tok->next;
                int64_t val = expect_number();
                add_constant(name_tok->str, val);
                // ラベルノードは不要なので削除
                cur = head.next;
                if (!cur) {
                    cur = &head;
                } else {
                    // 最後のノードの一つ前を見つける
                    Insn *prev = &head;
                    while (prev->next && prev->next->next) prev = prev->next;
                    cur = prev;
                    cur->next = NULL; // ラベルノードを削除
                }
            }

            skip_newlines();
            continue;
        }

        // db ディレクティブ（データセクション内）
        if (tok->kind == TK_IDENT && strcmp(tok->str, "db") == 0) {
            tok = tok->next;
            while (1) {
                if (tok->kind == TK_STRING) {
                    for (int i = 0; tok->str[i]; i++) {
                        emit_data((uint8_t)tok->str[i]);
                    }
                    tok = tok->next;
                } else if (tok->kind == TK_NUM || tok->kind == TK_MINUS) {
                    int64_t v = expect_number();
                    emit_data((uint8_t)(v & 0xFF));
                } else {
                    break;
                }
                if (tok->kind == TK_COMMA) {
                    tok = tok->next;
                } else {
                    break;
                }
            }
            skip_newlines();
            continue;
        }

        if (tok->kind != TK_IDENT) {
            // 空行など
            skip_newlines();
            continue;
        }

        Token *mnemonic = tok;
        tok = tok->next;

        // syscall
        if (strcmp(mnemonic->str, "syscall") == 0) {
            cur->next = new_insn(IN_SYSCALL);
            cur = cur->next;
            skip_newlines();
            continue;
        }

        // ret
        if (strcmp(mnemonic->str, "ret") == 0) {
            cur->next = new_insn(IN_RET);
            cur = cur->next;
            skip_newlines();
            continue;
        }

        // push
        if (strcmp(mnemonic->str, "push") == 0) {
            Token *reg_tok = expect_ident();
            const RegInfo *ri = find_reg(reg_tok->str);
            if (!ri) error("line %d: unknown register '%s'", reg_tok->line, reg_tok->str);
            Insn *in = new_insn(IN_PUSH);
            in->reg1 = ri->code;
            in->reg1_ext = ri->rex_ext;
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        // pop
        if (strcmp(mnemonic->str, "pop") == 0) {
            Token *reg_tok = expect_ident();
            const RegInfo *ri = find_reg(reg_tok->str);
            if (!ri) error("line %d: unknown register '%s'", reg_tok->line, reg_tok->str);
            Insn *in = new_insn(IN_POP);
            in->reg1 = ri->code;
            in->reg1_ext = ri->rex_ext;
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        // jmp
        if (strcmp(mnemonic->str, "jmp") == 0) {
            Token *label_tok = expect_ident();
            Insn *in = new_insn(IN_JMP);
            strncpy(in->label, label_tok->str, 127);
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        // jcc (条件ジャンプ)
        const JccInfo *jcc = find_jcc(mnemonic->str);
        if (jcc) {
            Token *label_tok = expect_ident();
            Insn *in = new_insn(IN_JCC);
            in->jcc_opcode = jcc->cc;
            strncpy(in->label, label_tok->str, 127);
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        // call
        if (strcmp(mnemonic->str, "call") == 0) {
            Token *label_tok = expect_ident();
            Insn *in = new_insn(IN_CALL);
            strncpy(in->label, label_tok->str, 127);
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        // cmp
        if (strcmp(mnemonic->str, "cmp") == 0) {
            Token *dst_tok = expect_ident();
            const RegInfo *dst = find_reg(dst_tok->str);
            if (!dst) error("line %d: unknown register '%s'", dst_tok->line, dst_tok->str);
            expect(TK_COMMA);

            if (tok->kind == TK_IDENT) {
                const RegInfo *src = find_reg(tok->str);
                if (src) {
                    tok = tok->next;
                    Insn *in = new_insn(IN_CMP_REG_REG);
                    in->reg1 = dst->code;
                    in->reg1_ext = dst->rex_ext;
                    in->reg2 = src->code;
                    in->reg2_ext = src->rex_ext;
                    cur->next = in;
                    cur = in;
                    skip_newlines();
                    continue;
                }
                // Could be a constant
                char label[128];
                int64_t v = parse_imm_or_const(label);
                if (label[0]) error("line %d: cannot use label in cmp immediate", tok->line);
                Insn *in = new_insn(IN_CMP_REG_IMM);
                in->reg1 = dst->code;
                in->reg1_ext = dst->rex_ext;
                in->imm = v;
                cur->next = in;
                cur = in;
            } else {
                int64_t v = expect_number();
                Insn *in = new_insn(IN_CMP_REG_IMM);
                in->reg1 = dst->code;
                in->reg1_ext = dst->rex_ext;
                in->imm = v;
                cur->next = in;
                cur = in;
            }
            skip_newlines();
            continue;
        }

        // ALU ops: add, sub, xor, and, or
        const AluInfo *alu = find_alu(mnemonic->str);
        if (alu) {
            Token *dst_tok = expect_ident();
            const RegInfo *dst = find_reg(dst_tok->str);
            if (!dst) error("line %d: unknown register '%s'", dst_tok->line, dst_tok->str);
            expect(TK_COMMA);

            if (tok->kind == TK_IDENT) {
                const RegInfo *src = find_reg(tok->str);
                if (src) {
                    tok = tok->next;
                    Insn *in = new_insn(IN_ALU_REG_REG);
                    in->reg1 = dst->code;
                    in->reg1_ext = dst->rex_ext;
                    in->reg2 = src->code;
                    in->reg2_ext = src->rex_ext;
                    in->alu_opcode = alu->opcode;
                    cur->next = in;
                    cur = in;
                    skip_newlines();
                    continue;
                }
                // constant
                char label[128];
                int64_t v = parse_imm_or_const(label);
                if (label[0]) error("line %d: cannot use label in ALU immediate", tok->line);
                Insn *in = new_insn(IN_ALU_REG_IMM);
                in->reg1 = dst->code;
                in->reg1_ext = dst->rex_ext;
                in->imm = v;
                in->alu_opcode = alu->opcode;
                in->jcc_opcode = alu->imm_ext; // reuse for /r
                cur->next = in;
                cur = in;
            } else {
                int64_t v = expect_number();
                Insn *in = new_insn(IN_ALU_REG_IMM);
                in->reg1 = dst->code;
                in->reg1_ext = dst->rex_ext;
                in->imm = v;
                in->alu_opcode = alu->opcode;
                in->jcc_opcode = alu->imm_ext;
                cur->next = in;
                cur = in;
            }
            skip_newlines();
            continue;
        }

        // mov
        if (strcmp(mnemonic->str, "mov") == 0) {
            // mov dst, src
            // dst can be: reg, [reg], [reg + disp]
            // src can be: reg, imm, [reg], [reg + disp], label

            if (tok->kind == TK_LBRACKET) {
                // mov [mem], reg
                tok = tok->next;
                int base, base_ext;
                int32_t disp;
                int has_disp;
                parse_mem_operand(&base, &base_ext, &disp, &has_disp);
                expect(TK_COMMA);
                Token *src_tok = expect_ident();
                const RegInfo *src = find_reg(src_tok->str);
                if (!src) error("line %d: unknown register '%s'", src_tok->line, src_tok->str);

                Insn *in = new_insn(IN_MOV_MEM_REG);
                in->mem_base = base;
                in->mem_base_ext = base_ext;
                in->mem_disp = disp;
                in->has_disp = has_disp;
                in->reg1 = src->code;
                in->reg1_ext = src->rex_ext;
                cur->next = in;
                cur = in;
                skip_newlines();
                continue;
            }

            Token *dst_tok = expect_ident();
            const RegInfo *dst = find_reg(dst_tok->str);
            if (!dst) error("line %d: unknown register '%s'", dst_tok->line, dst_tok->str);
            expect(TK_COMMA);

            if (tok->kind == TK_LBRACKET) {
                // mov reg, [mem]
                tok = tok->next;
                int base, base_ext;
                int32_t disp;
                int has_disp;
                parse_mem_operand(&base, &base_ext, &disp, &has_disp);

                Insn *in = new_insn(IN_MOV_REG_MEM);
                in->reg1 = dst->code;
                in->reg1_ext = dst->rex_ext;
                in->mem_base = base;
                in->mem_base_ext = base_ext;
                in->mem_disp = disp;
                in->has_disp = has_disp;
                cur->next = in;
                cur = in;
                skip_newlines();
                continue;
            }

            if (tok->kind == TK_IDENT) {
                // mov reg, reg  or  mov reg, label  or  mov reg, constant
                const RegInfo *src = find_reg(tok->str);
                if (src) {
                    tok = tok->next;
                    Insn *in = new_insn(IN_MOV_REG_REG);
                    in->reg1 = dst->code;
                    in->reg1_ext = dst->rex_ext;
                    in->reg2 = src->code;
                    in->reg2_ext = src->rex_ext;
                    cur->next = in;
                    cur = in;
                    skip_newlines();
                    continue;
                }
                // constant or label
                char label[128];
                int64_t v = parse_imm_or_const(label);
                if (label[0]) {
                    // ラベル参照 → mov reg, label (アドレスロード)
                    Insn *in = new_insn(IN_MOV_REG_LABEL);
                    in->reg1 = dst->code;
                    in->reg1_ext = dst->rex_ext;
                    strncpy(in->label, label, 127);
                    cur->next = in;
                    cur = in;
                } else {
                    Insn *in = new_insn(IN_MOV_REG_IMM);
                    in->reg1 = dst->code;
                    in->reg1_ext = dst->rex_ext;
                    in->imm = v;
                    cur->next = in;
                    cur = in;
                }
                skip_newlines();
                continue;
            }

            // mov reg, imm
            int64_t v = expect_number();
            Insn *in = new_insn(IN_MOV_REG_IMM);
            in->reg1 = dst->code;
            in->reg1_ext = dst->rex_ext;
            in->imm = v;
            cur->next = in;
            cur = in;
            skip_newlines();
            continue;
        }

        error("line %d: unknown instruction '%s'", mnemonic->line, mnemonic->str);
    }

    return head.next;
}

// ============================================================
// ModR/M + SIB エンコーディングヘルパー
// ============================================================

// メモリオペランドのサイズ計算（ModR/M + optional SIB + optional disp）
static int mem_operand_size(int base, int disp, int has_disp) {
    int size = 1; // ModR/M
    // rsp/r12 は SIB が必要
    if ((base & 7) == 4) size++; // SIB
    // rbp/r13 で mod=00 は使えない → mod=01 (disp8=0) にする
    if (!has_disp && ((base & 7) == 5)) {
        has_disp = 1;
        disp = 0;
    }
    if (has_disp) {
        if (disp >= -128 && disp <= 127) {
            size += 1; // disp8
        } else {
            size += 4; // disp32
        }
    }
    return size;
}

// メモリオペランドのエンコード: reg は /r フィールド (上位3ビット)
static void emit_mem_operand(int reg, int base, int disp, int has_disp) {
    // rbp/r13 で mod=00 は [rip+disp32] になってしまうので、mod=01 disp8=0 を使う
    if (!has_disp && ((base & 7) == 5)) {
        has_disp = 1;
        disp = 0;
    }

    int mod;
    if (!has_disp) {
        mod = 0;
    } else if (disp >= -128 && disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }

    if ((base & 7) == 4) {
        // rsp/r12: SIB required
        emit((mod << 6) | ((reg & 7) << 3) | 4); // ModR/M with SIB escape
        emit((4 << 3) | (base & 7));              // SIB: index=none(rsp), base=rsp
    } else {
        emit((mod << 6) | ((reg & 7) << 3) | (base & 7));
    }

    if (mod == 1) {
        emit((uint8_t)(int8_t)disp);
    } else if (mod == 2) {
        emit_le32((uint32_t)disp);
    }
}

// ============================================================
// Pass 1: 命令サイズ計算 + ラベルオフセット確定
// ============================================================

static void pass1(Insn *insns) {
    int offset = 0;
    for (Insn *in = insns; in; in = in->next) {
        in->offset = offset;
        switch (in->kind) {
        case IN_LABEL:
            in->size = 0;
            if (in->imm) {
                // data label: パース時に記録した data_offset を使用
                add_symbol(in->label, 0, 1, in->mem_disp);
            } else {
                add_symbol(in->label, offset, 0, 0);
            }
            break;
        case IN_MOV_REG_IMM:
            // REX.W + C7 + ModR/M + imm32 = 7 bytes
            in->size = 7;
            if (in->reg1_ext) in->size = 7; // REX prefix already counted
            break;
        case IN_MOV_REG_REG:
            // REX.W + 89 + ModR/M = 3 bytes
            in->size = 3;
            break;
        case IN_MOV_REG_LABEL:
            // movabs r64, imm64: REX.W + B8+rd + imm64 = 10 bytes
            in->size = 10;
            break;
        case IN_MOV_REG_MEM:
        case IN_MOV_MEM_REG:
            // REX.W + 8B/89 + mem_operand
            in->size = 2 + mem_operand_size(in->mem_base, in->mem_disp, in->has_disp);
            break;
        case IN_ALU_REG_REG:
            // REX.W + opcode + ModR/M = 3 bytes
            in->size = 3;
            break;
        case IN_ALU_REG_IMM:
            // REX.W + 81 + ModR/M + imm32 = 7 bytes
            in->size = 7;
            break;
        case IN_CMP_REG_IMM:
            // REX.W + 81 + ModR/M(/7) + imm32 = 7 bytes
            in->size = 7;
            break;
        case IN_CMP_REG_REG:
            // REX.W + 39 + ModR/M = 3 bytes
            in->size = 3;
            break;
        case IN_PUSH:
            in->size = in->reg1_ext ? 2 : 1; // REX.B + 50+reg or just 50+reg
            break;
        case IN_POP:
            in->size = in->reg1_ext ? 2 : 1;
            break;
        case IN_RET:
            in->size = 1;
            break;
        case IN_SYSCALL:
            in->size = 2;
            break;
        case IN_JMP:
            in->size = 5; // E9 + rel32
            break;
        case IN_JCC:
            in->size = 6; // 0F 8x + rel32
            break;
        case IN_CALL:
            in->size = 5; // E8 + rel32
            break;
        }
        offset += in->size;
    }
}

// ============================================================
// Pass 2: 機械語生成
// ============================================================

static uint64_t text_base_addr; // .text のベースアドレス（ELFヘッダ後）
static uint64_t data_base_addr; // .data のベースアドレス

static void pass2(Insn *insns) {
    for (Insn *in = insns; in; in = in->next) {
        switch (in->kind) {
        case IN_LABEL:
            break;

        case IN_MOV_REG_IMM: {
            // REX.W prefix
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x41; // REX.B
            emit(rex);
            emit(0xC7);
            emit(0xC0 | (in->reg1 & 7));
            emit_le32((uint32_t)in->imm);
            break;
        }

        case IN_MOV_REG_REG: {
            uint8_t rex = 0x48;
            if (in->reg2_ext) rex |= 0x44; // REX.R
            if (in->reg1_ext) rex |= 0x41; // REX.B
            emit(rex);
            emit(0x89); // MOV r/m64, r64
            emit(0xC0 | ((in->reg2 & 7) << 3) | (in->reg1 & 7));
            break;
        }

        case IN_MOV_REG_LABEL: {
            // movabs r64, imm64: REX.W + B8+rd + imm64
            Symbol *sym = find_symbol(in->label);
            if (!sym) error("undefined label '%s'", in->label);
            uint64_t addr;
            if (sym->is_data) {
                addr = data_base_addr + sym->data_offset;
            } else {
                addr = text_base_addr + sym->offset;
            }
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x41;
            emit(rex);
            emit(0xB8 | (in->reg1 & 7));
            // emit 64-bit immediate
            for (int i = 0; i < 8; i++) {
                emit((addr >> (i * 8)) & 0xFF);
            }
            break;
        }

        case IN_MOV_REG_MEM: {
            // MOV r64, [base + disp]: REX.W + 8B + ModR/M
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x44; // REX.R
            if (in->mem_base_ext) rex |= 0x41; // REX.B
            emit(rex);
            emit(0x8B);
            emit_mem_operand(in->reg1, in->mem_base, in->mem_disp, in->has_disp);
            break;
        }

        case IN_MOV_MEM_REG: {
            // MOV [base + disp], r64: REX.W + 89 + ModR/M
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x44; // REX.R (src reg)
            if (in->mem_base_ext) rex |= 0x41; // REX.B (base reg)
            emit(rex);
            emit(0x89);
            emit_mem_operand(in->reg1, in->mem_base, in->mem_disp, in->has_disp);
            break;
        }

        case IN_ALU_REG_REG: {
            uint8_t rex = 0x48;
            if (in->reg2_ext) rex |= 0x44; // REX.R
            if (in->reg1_ext) rex |= 0x41; // REX.B
            emit(rex);
            emit(in->alu_opcode);
            emit(0xC0 | ((in->reg2 & 7) << 3) | (in->reg1 & 7));
            break;
        }

        case IN_ALU_REG_IMM: {
            // REX.W + 81 + ModR/M(11, /r, rm) + imm32
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x41;
            emit(rex);
            emit(0x81);
            emit(0xC0 | (in->jcc_opcode << 3) | (in->reg1 & 7)); // jcc_opcode reused as /r
            emit_le32((uint32_t)in->imm);
            break;
        }

        case IN_CMP_REG_IMM: {
            uint8_t rex = 0x48;
            if (in->reg1_ext) rex |= 0x41;
            emit(rex);
            emit(0x81);
            emit(0xC0 | (7 << 3) | (in->reg1 & 7)); // /7
            emit_le32((uint32_t)in->imm);
            break;
        }

        case IN_CMP_REG_REG: {
            uint8_t rex = 0x48;
            if (in->reg2_ext) rex |= 0x44;
            if (in->reg1_ext) rex |= 0x41;
            emit(rex);
            emit(0x39);
            emit(0xC0 | ((in->reg2 & 7) << 3) | (in->reg1 & 7));
            break;
        }

        case IN_PUSH:
            if (in->reg1_ext) emit(0x41); // REX.B
            emit(0x50 | (in->reg1 & 7));
            break;

        case IN_POP:
            if (in->reg1_ext) emit(0x41);
            emit(0x58 | (in->reg1 & 7));
            break;

        case IN_RET:
            emit(0xC3);
            break;

        case IN_SYSCALL:
            emit(0x0F);
            emit(0x05);
            break;

        case IN_JMP: {
            Symbol *sym = find_symbol(in->label);
            if (!sym) error("undefined label '%s'", in->label);
            int32_t rel = sym->offset - (in->offset + 5);
            emit(0xE9);
            emit_le32((uint32_t)rel);
            break;
        }

        case IN_JCC: {
            Symbol *sym = find_symbol(in->label);
            if (!sym) error("undefined label '%s'", in->label);
            int32_t rel = sym->offset - (in->offset + 6);
            emit(0x0F);
            emit(in->jcc_opcode);
            emit_le32((uint32_t)rel);
            break;
        }

        case IN_CALL: {
            Symbol *sym = find_symbol(in->label);
            if (!sym) error("undefined label '%s'", in->label);
            int32_t rel = sym->offset - (in->offset + 5);
            emit(0xE8);
            emit_le32((uint32_t)rel);
            break;
        }
        }
    }
}

// ============================================================
// ELF 出力
// ============================================================

static void write_le16(FILE *f, uint16_t v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

static void write_le32(FILE *f, uint32_t v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 24) & 0xFF, f);
}

static void write_le64(FILE *f, uint64_t v) {
    for (int i = 0; i < 8; i++)
        fputc((v >> (i * 8)) & 0xFF, f);
}

static void write_elf(const char *filename) {
    FILE *f = fopen(filename, "wb");
    if (!f) error("cannot open '%s'", filename);

    uint64_t base = 0x400000;
    int ehdr_size = 64;
    int phdr_size = 56;
    int num_phdrs = (data_len > 0) ? 2 : 1;
    int headers_size = ehdr_size + phdr_size * num_phdrs;

    // .text starts right after headers
    uint64_t text_offset = headers_size;
    uint64_t text_vaddr = base + text_offset;
    int text_size = code_len;

    // .data starts after .text, page-aligned in virtual memory
    // For simplicity, place data right after text in file, but at aligned vaddr
    uint64_t data_offset = text_offset + text_size;
    // Align data to 0x1000 boundary in virtual memory
    uint64_t data_vaddr = ((text_vaddr + text_size + 0xFFF) & ~0xFFFULL);
    // But we need file offset to match: p_offset mod p_align == p_vaddr mod p_align
    // Since we use p_align=0x1000, adjust data_offset so offset%0x1000 == vaddr%0x1000
    // Simplest: make data_offset also aligned so that data_offset == data_vaddr - base
    // Actually, let's just put everything contiguously and use the same base mapping trick
    // We need: p_vaddr - p_offset to be a multiple of p_align
    // With data_vaddr aligned and data_offset = text_offset + text_size:
    // (data_vaddr - data_offset) needs to be multiple of 0x1000
    // data_vaddr = ((base + text_offset + text_size + 0xFFF) & ~0xFFF)
    // data_offset = text_offset + text_size
    // data_vaddr - data_offset = data_vaddr - (text_offset + text_size)
    // Let's verify: data_vaddr = ((base + data_offset + 0xFFF) & ~0xFFF)
    // This means data_vaddr >= base + data_offset
    // data_vaddr - data_offset >= base, and (data_vaddr - data_offset) - base < 0x1000
    // Since base is 0x400000 which is page-aligned, data_vaddr - data_offset is a multiple of 0x1000 only if
    // (data_vaddr - data_offset) mod 0x1000 == 0
    // This isn't guaranteed. Let's use a different approach:
    // Make data_offset page-aligned in the file as well
    if (data_len > 0) {
        data_offset = (data_offset + 0xFFF) & ~0xFFFULL;
        data_vaddr = base + data_offset;
    }

    text_base_addr = text_vaddr;
    data_base_addr = data_vaddr;

    uint64_t entry = text_vaddr;
    uint64_t total_file_size = (data_len > 0) ? (data_offset + data_len) : (text_offset + text_size);

    // === ELF Header (64 bytes) ===
    // e_ident
    fputc(0x7F, f); fputc('E', f); fputc('L', f); fputc('F', f);
    fputc(2, f);    // 64-bit
    fputc(1, f);    // little endian
    fputc(1, f);    // ELF version
    fputc(0, f);    // OS/ABI
    for (int i = 0; i < 8; i++) fputc(0, f); // padding

    write_le16(f, 2);              // e_type: ET_EXEC
    write_le16(f, 0x3E);          // e_machine: EM_X86_64
    write_le32(f, 1);             // e_version
    write_le64(f, entry);         // e_entry
    write_le64(f, ehdr_size);     // e_phoff
    write_le64(f, 0);             // e_shoff (no section headers)
    write_le32(f, 0);             // e_flags
    write_le16(f, ehdr_size);     // e_ehsize
    write_le16(f, phdr_size);     // e_phentsize
    write_le16(f, num_phdrs);     // e_phnum
    write_le16(f, 0);             // e_shentsize
    write_le16(f, 0);             // e_shnum
    write_le16(f, 0);             // e_shstrndx

    // === Program Header 1: .text (RX) ===
    write_le32(f, 1);             // p_type: PT_LOAD
    write_le32(f, 5);             // p_flags: PF_R | PF_X
    write_le64(f, 0);             // p_offset: load from start of file
    write_le64(f, base);          // p_vaddr
    write_le64(f, base);          // p_paddr
    if (data_len > 0) {
        write_le64(f, text_offset + text_size); // p_filesz
        write_le64(f, text_offset + text_size); // p_memsz
    } else {
        write_le64(f, total_file_size); // p_filesz
        write_le64(f, total_file_size); // p_memsz
    }
    write_le64(f, 0x1000);        // p_align

    // === Program Header 2: .data (RW) ===
    if (data_len > 0) {
        write_le32(f, 1);             // p_type: PT_LOAD
        write_le32(f, 6);             // p_flags: PF_R | PF_W
        write_le64(f, data_offset);   // p_offset
        write_le64(f, data_vaddr);    // p_vaddr
        write_le64(f, data_vaddr);    // p_paddr
        write_le64(f, data_len);      // p_filesz
        write_le64(f, data_len);      // p_memsz
        write_le64(f, 0x1000);        // p_align
    }

    // === .text ===
    fwrite(code_buf, 1, text_size, f);

    // === padding before .data ===
    if (data_len > 0) {
        long cur_pos = ftell(f);
        long pad = data_offset - cur_pos;
        for (long i = 0; i < pad; i++) fputc(0, f);
        // === .data ===
        fwrite(data_buf, 1, data_len, f);
    }

    fclose(f);
}

// ============================================================
// 入力読み込み
// ============================================================

static char *read_all_stdin(void) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    while (1) {
        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0) break;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    buf[len] = '\0';
    return buf;
}

// ============================================================
// main
// ============================================================

int main(void) {
    char *src = read_all_stdin();

    Token *tokens = tokenize(src);
    Insn *insns = parse(tokens);

    // Pass 1: サイズ計算 + ラベル登録
    pass1(insns);

    // ELFヘッダサイズを計算してベースアドレスを設定
    int num_phdrs = (data_len > 0) ? 2 : 1;
    int headers_size = 64 + 56 * num_phdrs;
    text_base_addr = 0x400000 + headers_size;

    if (data_len > 0) {
        uint64_t data_offset = ((headers_size + code_len + 0xFFF) & ~0xFFFULL);
        data_base_addr = 0x400000 + data_offset;
    }

    // Pass 2: 機械語生成
    pass2(insns);

    // ELF出力
    write_elf("a.out");

    return 0;
}
