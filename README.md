# Whippet

Ethernos Studio 的独立编译器后端。接收前端产出的 Whippet IR，负责中端优化与
x86-64 代码生成。设计见 [design/WHIPPET_DESIGN.md](design/WHIPPET_DESIGN.md)，
M0 脚手架见 [design/WHIPPET_M0_SCAFFOLD.md](design/WHIPPET_M0_SCAFFOLD.md)。

作者：dhjs0000

许可证：[MIT](LICENSE)。

当前仓库已完成 **M0**：

- Whippet IR 文本（`.wir`）的完整词法/语法解析；
- 严格校验：未知 opcode/类型/谓词、重复定义、未定义引用、phi 位置、块终止、
  类型与 cast 合法性、直接调用签名；
- canonical 打印器，`读入 -> 打印` 逐字节无损；
- 空发射层（M0 输出 canonical IR，M1 起输出 GNU as 文本）；
- `ctest` 覆盖 round-trip 与 20 个错误定位用例；
- arena + 24 B IR 节点 + u32 句柄（符合 §9.2 内存预算）。

## 目录

```
whippet/
├── CMakeLists.txt
├── include/whippet/        # 公共 API：ir.h、emit.h
├── src/
│   ├── ir/                 # M0：arena、节点池、解析、校验、打印
│   ├── mid/passes.c        # M3：-O1 pass 流水线（占位）
│   ├── isel/isel.c         # M1：指令选择（占位）
│   ├── regalloc/linear.c   # M1：线性扫描（占位）
│   ├── emit/as.c           # M0：文本发射；M1 起发 GNU as
│   └── cli/main.c          # whippet 命令行
├── tests/                  # round-trip / 诊断测试 + canonical 用例
├── docs/IR_TEXT.md         # M0 实现的 IR 文本契约（语法参考）
└── design/                 # 设计文档随仓库走
```

## 构建

编译器统一用 Clang，生成器用 Ninja（与设计 §11 一致）。

### Arch / Linux

```bash
sudo pacman -S --needed cmake ninja clang
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows

先进入 Visual Studio 开发者环境（提供 Windows SDK / MSVC 链接器；编译器仍用
clang），再配置。Ninja 若不在 `PATH`，用 `-DCMAKE_MAKE_PROGRAM=<ninja.exe>` 指定：

```powershell
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure
```

本机（Visual Studio 装在 `E:\VisualStudio`、LLVM 在 `E:\LLVM`、Ninja 由 VS 附带）
可用：

```powershell
cmd /c "call E:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat && E:\Python\Python315\Scripts\cmake.exe -B build -G Ninja -DCMAKE_C_COMPILER=E:/LLVM/bin/clang.exe -DCMAKE_MAKE_PROGRAM=E:/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
cmd /c "call E:\VisualStudio\VC\Auxiliary\Build\vcvars64.bat && E:\Python\Python315\Scripts\cmake.exe --build build"
cmd /c "E:\Python\Python315\Scripts\ctest.exe --test-dir build --output-on-failure"
```

### Sanitizer（合并前必跑）

```bash
cmake -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang -DWHIPPET_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Windows 下 CMake 会把 Clang 的 `clang_rt.asan_dynamic-x86_64.dll` 自动复制到
可执行文件旁边，无需改 `PATH`。

## 命令行

```text
whippet [--check] [-o <file>] <file.wir>
```

- 默认：解析、校验、打印 canonical IR 到 stdout（M0 的“空发射”）；
- `-o <file>`：写文件而不是 stdout（二进制写，保证 LF）；
- `--check`：只解析校验；
- 出错时输出 `path:line:column: error: message`，并打印对应源码行与 `^`。

```console
$ whippet tests/cases/basic.wir
func @add_mul(i32 %a, i32 %b) -> i32 {
entry:
  %v0 = mul i32 %a, 4
  %v1 = add i32 %v0, %b
  ret i32 %v1
}
```

## 测试

```console
$ ctest --test-dir build --output-on-failure
    Start 1: ir_roundtrip
1/6 Test #1: ir_roundtrip .....................   Passed
    Start 2: ir_diagnostics
2/6 Test #2: ir_diagnostics ...................   Passed
    Start 3: ir_metadata
3/6 Test #3: ir_metadata ......................   Passed
    Start 4: ir_normalisation
4/6 Test #4: ir_normalisation .................   Passed
    Start 5: cli_emit
5/6 Test #5: cli_emit .........................   Passed
    Start 6: cli_emit_matches
6/6 Test #6: cli_emit_matches .................   Passed
```

- `tests/cases/*.wir` 以 canonical 形式保存，round-trip 必须逐字节一致；
- `test_ir_errors` 断言非法输入被拒绝，并核对诊断行列；
- `test_ir_metadata` 检查 opcode/类型/谓词表自洽与空模块；
- `test_ir_normalize` 验证 BOM/注释/CRLF/十六进制/指数浮点的归一化；
- `cli_emit` 端到端验证 CLI 写出的文件与 canonical 用例逐字节一致。

## IR 文本契约

M0 实现的完整语法、类型规则、canonical 格式与已知边界见
[docs/IR_TEXT.md](docs/IR_TEXT.md)。要点：

- 七种操作数类型 `i8/i16/i32/i64/f32/f64/ptr`，`void` 仅用于返回值；
- 无 `i1`：比较产生 `i32` 0/1，`condbr` 对任意非零整数为真；
- SSA、显式基本块、`phi` 仅出现在块首；
- 跳转表 `switch` 是一等指令；
- 前端负责 SSA 构建、类型降级、控制流展平、语义裁决与聚合返回值（§3.2）；
- 解析器只做“海关”：拒绝畸形输入，不做前端语义的补救。

## 工程纪律

1. IR 存储一律走 arena（`WhippetArena`），节点/操作数/名字均不散落 `malloc`；
2. `-DWHIPPET_SANITIZE=ON` 下测试全绿才算完成；
3. 差分测试从 M1 起建（随机 C 程序 vs gcc/clang 输出）。

M0 的解析器还会检查 SSA 名字/块引用的存在性与类型一致性；**支配关系**
（dominance）仍由前端保证，后端不重建 SSA。
