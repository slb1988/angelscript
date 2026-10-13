# AngelScript 官方示例学习路线

8 个示例按难度递进排列，每个目录里有详细的 README.md 拆解知识点和底层原理。建议按以下顺序学习：

| 顺序 | 示例 | 主题 | 核心知识点 |
|------|------|------|-----------|
| 1 | [tutorial](tutorial/) | 最小完整流程 | 引擎生命周期、函数注册、编译、执行、传参/返回值、超时保护 |
| 2 | [console](console/) | 交互式控制台 | 全局属性绑定、重载注册、增量编译、引擎自省（反射） |
| 3 | [events](events/) | 事件驱动执行 | Suspend/Resume 时间片调度、多 context 协作、Abort |
| 4 | [coroutine](coroutine/) | 协程 | yield/createCoRoutine、funcdef、dictionary 传参、contextmgr |
| 5 | [concurrent](concurrent/) | 协作式并发 | 多模块隔离、sleep 调度、绿色线程模型 |
| 6 | [asbuild](asbuild/) | 离线编译器 | 配置文件驱动的接口注册、字符串工厂、字节码序列化 |
| 7 | [asrun](asrun/) | 命令行脚本运行器 | scriptbuilder 预处理、调试器、context 池、#pragma 回调 |
| 8 | [game](game/) | 完整游戏集成 | 引用类型注册、weakref、脚本类实现接口、跨模块 shared 类型、消息传递 |

## 通用构建方法（VS 2026）

所有示例都依赖预编译的核心库，**先编库再编示例**：

```powershell
$msbuild = 'D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
# 1. 核心库（产出 sdk/angelscript/lib/*.lib）
& $msbuild ..\angelscript\projects\msvc2026\angelscript.sln /p:Configuration=Debug /p:Platform=x64 /m
# 2. 示例（以 tutorial 为例，exe 输出到示例的 bin/）
& $msbuild tutorial\projects\msvc2026\tutorial.sln /p:Configuration=Debug /p:Platform=x64 /m
```

**运行时必须以示例的 `bin/` 为工作目录**（脚本 .as 文件按相对路径加载）。在 VS 里调试请设置：项目属性 → Debugging → Working Directory = `$(ProjectDir)..\..\bin`。

## 三条主线

读这些示例时，始终围绕三条主线理解 AngelScript 的设计：

1. **宿主暴露什么给脚本**（注册 API）：脚本没有任何内置能力，能调用的每个函数、能用的每个类型都是宿主用 `Register*` 系列接口注册的。从 tutorial 的一个全局函数，到 game 的完整引用类型 + 接口，注册复杂度逐级上升。
2. **脚本如何被编译**（模块系统）：`asIScriptModule` 是编译单元兼命名空间。`AddScriptSection` 收集源码 → `Build()` 触发 `asCBuilder` 两遍编译（先声明后函数体）→ 产物是字节码。asbuild 展示了编译产物可以序列化落盘。
3. **脚本如何被执行**（context 模型）：`asIScriptContext` 是一个独立的虚拟机执行栈。`Prepare → SetArg → Execute → GetReturn` 是基本节奏；`Suspend/Resume` 支撑了 events/coroutine/concurrent 三个示例的全部调度魔法——AngelScript 的"多线程"其实是单线程上的多个可暂停的 VM 栈。
