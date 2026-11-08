# Whippet M0 开工脚手架

> 配套 WHIPPET_DESIGN.md v0.6 使用。复制代码块即可建仓库。
> 目标：空仓库 → `cmake --build` 通过、`ctest` 跑通空壳测试 → M0 直接写 `src/ir/`。

## 目录结构

```
whippet/
├── CMakeLists.txt
├── include/
│   └── whippet/
│       └── ir.h          # M0：IR 节点定义、句柄类型
├── src/
│   ├── ir/
│   │   ├── ir.c          # 节点池/arena
│   │   ├── parse.c       # .wir 文本解析
│   │   └── print.c       # 打印器（round-trip 的另一半）
│   ├── mid/
│   │   └── passes.c      # M0 留空，M3 填 O1 流水线
│   ├── isel/
│   │   └── isel.c        # M1 开工
│   ├── regalloc/
│   │   └── linear.c      # M1 开工
│   ├── emit/
│   │   └── as.c          # M0：空发射（打印到 stdout）
│   └── cli/
│       └── main.c        # whippet 命令行入口
├── tests/
│   ├── CMakeLists.txt
│   └── test_ir_roundtrip.c
└── design/
    └── WHIPPET_DESIGN.md # 设计文档放这里，跟着仓库走
```

## 根 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(whippet VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(WHIPPET_SANITIZE "Enable ASan+UBSan (Clang/GCC only)" OFF)

add_library(whippet_core STATIC
    src/ir/ir.c
    src/ir/parse.c
    src/ir/print.c
    src/mid/passes.c
    src/isel/isel.c
    src/regalloc/linear.c
    src/emit/as.c
)
target_include_directories(whippet_core PUBLIC include)
target_compile_options(whippet_core PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(WHIPPET_SANITIZE)
    if(MSVC)
        message(WARNING "MSVC 仅支持 /fsanitize=address 且无 UBSan；建议调试档直接用 clang")
    else()
        target_compile_options(whippet_core PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(whippet_core PUBLIC -fsanitize=address,undefined)
    endif()
endif()

add_executable(whippet src/cli/main.c)
target_link_libraries(whippet PRIVATE whippet_core)

enable_testing()
add_subdirectory(tests)
```

## tests/CMakeLists.txt

```cmake
add_executable(test_ir_roundtrip test_ir_roundtrip.c)
target_link_libraries(test_ir_roundtrip PRIVATE whippet_core)
add_test(NAME ir_roundtrip COMMAND test_ir_roundtrip)
```

## 占位源文件（让脚手架一次编译通过）

`src/cli/main.c`：

```c
#include <stdio.h>

int main(void) {
    puts("whippet 0.1.0 (M0 scaffold)");
    return 0;
}
```

`tests/test_ir_roundtrip.c`：

```c
/* M0 验收：读 .wir -> 打印 -> 与原文逐字节比对 */
int main(void) { return 0; }
```

其余 `src/**/*.c` 各放一个空翻译单元即可（例如 `/* M0: TODO */`）——空文件是合法的 C 源文件。

## 两条工具链的开工命令

**Arch（主力机，G15 修好后）：**

```bash
sudo pacman -S --needed cmake ninja clang
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DWHIPPET_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

**Windows（备用机，cmake 用 TUNA 镜像装）：**

```powershell
winget install Ninja.Ninja LLVM.LLVM   # cmake 本体按 TUNA 镜像链接装
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DWHIPPET_SANITIZE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

说明：编译器统一用 clang 而不是 MSVC——§8 发射层要生成 GNU as 语法的汇编，clang/llvm-mc 就是这套语法体系的原生工具；MSVC(cl)只作为将来借用方兼容性的验证目标，不做开发主力。

## 三条工程纪律（来自设计文档 §11，CI 的硬门槛）

1. **一律 arena 分配**，禁止函数内散 malloc（发射缓冲等明确生命周期的例外需在代码评审时说得出理由）；
2. **`-DWHIPPET_SANITIZE=ON` 下测试全绿**才允许合并——ASan+UBSan 常驻；
3. **差分测试从 M1 起建**：随机 C 程序 → whippet 编译 vs gcc/clang 编译 → 比对输出。miscompile 只能靠对比防住，不能靠自觉。
