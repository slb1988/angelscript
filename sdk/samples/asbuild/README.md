# asbuild — 离线脚本编译器（字节码序列化）

**难度：★★★★☆　前置：tutorial，建议配合 asrun**

一个命令行工具：把 .as 源码编译成字节码文件，供发布版程序直接 `LoadByteCode` 加载——不用带编译器、不暴露源码、加载更快。核心难题是：**编译需要知道宿主注册了什么接口，但离线工具没有宿主的实现代码**。答案是配置文件驱动的"假注册"。

```
用法: asbuild <config file> <script file> <output>
示例: asbuild config.txt script.as script.abc
```

## 知识点拆解（对照 source/main.cpp）

### 1. 编译只需要签名，不需要实现（本示例的核心洞察）

`ConfigureEngine`（main.cpp:167）读取 config.txt，调用 scripthelper 的 **`ConfigEngineFromStream`**。config.txt 是一个 DSL（本目录 bin/config.txt，由 asrun 侧用 `WriteConfigToFile` 从真实引擎导出）：

```
ep 14 2                          // engine property
objtype "string" 1 ...           // 类型
func "void print(const string &in)"   // 函数——只有声明！
```

`ConfigEngineFromStream` 按这些声明注册类型和函数，函数指针指向一个**空的 generic 桩**。编译器做类型检查、重载决议只依赖签名，字节码里记录的是函数引用而非地址——所以"假注册"的引擎能产出与真实引擎完全兼容的字节码。运行侧（真实宿主）加载字节码时按签名重新绑定到真实现。

**工作流**：宿主程序用 `WriteConfigToFile` 导出接口 → asbuild 用它离线编译 → 宿主发布版 `LoadByteCode`。三方通过"声明字符串"这一契约解耦。

### 2. asIStringFactory：字符串常量的来源（main.cpp:107-163）

脚本里每个字符串字面量（`"hello"`）编译时都会调用宿主注册的字符串工厂：

```cpp
class CStdStringFactory : public asIStringFactory {
    const void *GetStringConstant(const char *data, asUINT length);  // 字面量 → 对象
    int ReleaseStringConstant(const void *str);                       // 引擎释放时回调
    int GetRawStringData(const void *str, char *data, asUINT *len);   // SaveByteCode 导出原文用
};
```

- 本实现用 map 做**去重缓存**（相同字面量全模块共享一个 string 对象 + 计数）；
- 为什么离线编译也需要它：字节码里的字符串常量表要通过 `GetRawStringData` 拿到原始字节写入文件；
- 这就是 `RegisterStdString` 内部注册的那个工厂——这里把它抄出来是为了教学（main.cpp:105 注释言明）。

### 3. 字节码序列化：asIBinaryStream（main.cpp:219-275)

```cpp
class CBytecodeStream : public asIBinaryStream {
    int Write(const void *ptr, asUINT size);   // SaveByteCode 的输出通道
    int Read(void *, asUINT);                  // LoadByteCode 用（这里不需要）
};
mod->SaveByteCode(&stream);
```

流接口由宿主实现——写文件、写内存、写网络都行。对应的加载侧是 `mod->LoadByteCode(&stream)`。

### 4. CScriptBuilder 首次登场（main.cpp:196-217）

之前示例手工 `AddScriptSection`；这里换用 **scriptbuilder** add_on：`StartNewModule → AddSectionFromFile → BuildModule`。它是带预处理器的加载器：处理 `#include`（自动去重）、`#if/#endif` 条件编译、提取元数据、`#pragma` 回调。asrun 与 game 都用它。

## 底层原理

- **SaveByteCode/LoadByteCode 的实现是 as_restore.cpp**（asCWriter/asCReader，引擎中最值得精读的文件之一）。要点：
  - 字节码不是原样落盘：指令里的函数指针、类型指针、字符串地址等运行期值被替换为**表索引**，加载时重新解析绑定——这就是跨进程、跨平台可用的原因；
  - 做了平台无关化：字节序、指针大小差异在读写两侧转换（32 位保存的字节码 64 位可加载）；
  - 加载时会校验模块引用的每个宿主注册项（按声明字符串匹配），**宿主接口对不上会加载失败**——所以 config.txt 与真实宿主必须同源。
- 想看格式细节：`asCWriter::Write`（as_restore.cpp）从模块头、函数表、类型表到字节码逐段写出，配合 `SetEngineProperty(asEP_COPY_SCRIPT_SECTIONS...)` 等选项还能控制是否保留调试信息。
- `engine->WriteMessage(...)`（main.cpp:178 等）：宿主主动向消息回调注入一条消息——让工具自身的错误与编译错误走同一输出通道，CLI 工具的良好实践。

## 构建运行

```powershell
& $msbuild projects\msvc2026\asbuild.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin
.\asbuild.exe config.txt script.as script.abc   # 产出字节码文件
```

## 思考题

1. script.abc 能被 tutorial 加载吗？（不能——tutorial 注册的接口与 config.txt 描述的不一致，LoadByteCode 校验会失败。接口契约是硬约束。）
2. 修改 config.txt 里某个函数的返回类型再编译会怎样？（编译按新签名通过，但真实宿主加载时绑定失败——体会"签名即 ABI"。）
3. 用十六进制查看器打开 script.abc，找找 "hello world" 字面量——然后去 as_restore.cpp 找到写入它的代码路径（提示：搜 stringConstants）。
