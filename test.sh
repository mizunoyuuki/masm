#!/bin/bash

# masm テストスクリプト
# 各ステップの検証を行う

PASS=0
FAIL=0

assert_exit() {
    local expected=$1
    local desc=$2
    shift 2
    local input="$*"

    echo "$input" | ./masm
    chmod +x a.out
    ./a.out
    local actual=$?

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc (exit=$actual)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected=$expected, actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
    rm -f a.out
}

assert_stdout() {
    local expected="$1"
    local desc="$2"
    shift 2
    local input="$*"

    echo "$input" | ./masm
    chmod +x a.out
    local actual
    actual=$(./a.out)

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected='$expected', actual='$actual')"
        FAIL=$((FAIL + 1))
    fi
    rm -f a.out
}

echo "=== Step 2: mov rax, imm ==="
assert_exit 0 "mov rax,60 → sys_exit(0)" \
"mov rax, 60
syscall"

echo ""
echo "=== Step 3: 複数行・複数レジスタ ==="
assert_exit 42 "exit(42)" \
"mov rax, 60
mov rdi, 42
syscall"

assert_exit 0 "exit(0) with rdi=0" \
"mov rax, 60
mov rdi, 0
syscall"

echo ""
echo "=== Step 5: ALU ops (xor, add, sub) ==="
assert_exit 0 "xor rdi,rdi → exit(0)" \
"mov rax, 60
xor rdi, rdi
syscall"

assert_exit 3 "add rdi, rcx" \
"mov rdi, 1
mov rcx, 2
add rdi, rcx
mov rax, 60
syscall"

assert_exit 5 "sub rdi, rcx" \
"mov rdi, 8
mov rcx, 3
sub rdi, rcx
mov rax, 60
syscall"

echo ""
echo "=== Step 6: push / pop / ret ==="
assert_exit 42 "push/pop rdi" \
"mov rdi, 42
push rdi
mov rdi, 0
pop rdi
mov rax, 60
syscall"

echo ""
echo "=== Step 7+8: ラベル + jmp ==="
assert_exit 0 "jmp skips mov rdi,99" \
"jmp end
mov rdi, 99
end:
mov rax, 60
xor rdi, rdi
syscall"

echo ""
echo "=== Step 9: cmp + 条件ジャンプ ==="
assert_exit 1 "cmp rdi,5; jg → exit(1)" \
"mov rdi, 10
cmp rdi, 5
jg greater
mov rax, 60
mov rdi, 0
syscall
greater:
mov rax, 60
mov rdi, 1
syscall"

assert_exit 0 "cmp rdi,5; jle → exit(0)" \
"mov rdi, 3
cmp rdi, 5
jg greater
mov rax, 60
mov rdi, 0
syscall
greater:
mov rax, 60
mov rdi, 1
syscall"

assert_exit 1 "je test" \
"mov rdi, 5
cmp rdi, 5
je equal
mov rax, 60
mov rdi, 0
syscall
equal:
mov rax, 60
mov rdi, 1
syscall"

echo ""
echo "=== Step 10: call / ret ==="
assert_exit 42 "call subroutine" \
"mov rdi, 42
call set_exit
syscall
set_exit:
mov rax, 60
ret"

echo ""
echo "=== Step 11: メモリオペランド ==="
assert_exit 42 "mov [rsp-8], rdi; mov rdi, [rsp-8]" \
"mov rdi, 42
sub rsp, 8
mov [rsp], rdi
mov rdi, 0
mov rdi, [rsp]
add rsp, 8
mov rax, 60
syscall"

echo ""
echo "=== Step 12: .data + 文字列出力 ==="
assert_stdout "Hello, world!" "Hello world" \
'section .data
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
syscall'

echo ""
echo "================================"
echo "Results: $PASS passed, $FAIL failed"
if [ $FAIL -ne 0 ]; then
    exit 1
fi
