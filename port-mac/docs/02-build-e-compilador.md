# 02: Build (CMake) e compilador

## Estado atual

- `CMakeLists.txt:16-18` aborta com `FATAL_ERROR "wowdecomp targets Windows only."` em qualquer sistema que não
  seja Windows.
- `CMakeLists.txt:43-49` define `WIN32`, `_WINDOWS` e `WIN32_LEAN_AND_MEAN` para todo o projeto. No Mac, `WIN32`
  não pode ser definido globalmente, porque terceiros (zlib, expat, FreeType, FMOD) usam essa macro para escolher
  código de plataforma.
- `WoW/Source/CMakeLists.txt` e `Tools/WowClient/CMakeLists.txt`:
  - `add_executable(... WIN32 ...)`;
  - opções de link `/INCREMENTAL:NO /ENTRY:start`;
  - fontes de plataforma escolhidas por glob de `Win64/*.cpp` ou `Win32/*.cpp`;
  - saída em `bin/Win`.
- `Engine/Source/Gx/CMakeLists.txt:18` linka `d3d9.lib` sem condição. A linha 3-5 já exclui os backends quando
  `NOT WIN32`, então fora do Windows não sobra nenhum device.
- `Engine/Source/Os/CMakeLists.txt` linka `imm32`, `wininet`, `ws2_32`, `dinput8`, `dxguid` e `version` dentro
  de `if(WIN32)`.
- Os subprojetos usam `file(GLOB_RECURSE "*.cpp")`, que pega os arquivos de `Win32/` e de `Win64/` ao mesmo tempo.
  Hoje isso funciona porque cada arquivo começa com `#if defined(WOWDECOMP_ARCH_64)`. No Mac, os arquivos `Win*/`
  inteiros precisam sair da lista (`list(FILTER ... EXCLUDE REGEX "/Win(32|64)/")`).
- Terceiros sob `if (WIN32)`: `nvapi`, `lgLcd` e `DivxDecoder`. O `xvidcore` é distribuído só como
  `lib/x64/libxvidcore.lib`.

## Mudanças propostas no CMake

1. Trocar o `FATAL_ERROR` por detecção de plataforma: `WOWDECOMP_PLATFORM_WIN` / `WOWDECOMP_PLATFORM_MAC`.
2. Pôr as definições `WIN32`/`_WINDOWS` dentro de `if (WIN32)`.
3. Seguir uma convenção de pastas por módulo: `Win32/`, `Win64/` e **`Mac/`**. Uma função CMake auxiliar
   (`wowdecomp_platform_sources(target)`) faz o glob só da pasta da plataforma atual.
4. Fazer um alvo `Wow` para Mac como `MACOSX_BUNDLE`, com `Info.plist` (inclui `NSMicrophoneUsageDescription`
   para o voice chat e `NSHighResolutionCapable`) e saída em `bin/Mac/World of Warcraft.app`.
5. Links no Mac: `-framework Cocoa`, `OpenGL` (depois `Metal`/`QuartzCore`), `CoreAudio`, `AudioToolbox`,
   `AudioUnit`, `IOKit`, `GameController`, `Carbon` (só para `TISCopyCurrentKeyboardLayoutInputSource`, usado no
   mapeamento de teclado) e `Security` (`SecRandomCopyBytes`).
6. `CMAKE_OSX_ARCHITECTURES=arm64`, só arm64 nativo (decisão registrada no doc 07). Sem x86_64 e sem
   universal binary.
7. Arquivos `.mm` (Objective-C++) só nas pastas `Mac/`, com `enable_language(OBJCXX)`.

## Header de compatibilidade

Criar `Engine/Source/Base/Platform/Compat.h`, ou equivalente, incluído antes de tudo nos alvos Mac (via
`-include` ou `target_precompile_headers`). Ele deve ser pequeno e cobrir só idiomas de compilador e CRT, não a
API Win32:

| MSVC | Mac |
|---|---|
| `__stdcall`, `__cdecl`, `__fastcall`, `__thiscall`, `WINAPI`, `CALLBACK` | macro vazia (no arm64 e x86_64 SysV não há convenções alternativas) |
| `__forceinline` | `inline __attribute__((always_inline))` |
| `__declspec(noinline)`, `__declspec(align(n))`, `__declspec(noreturn)` | `__attribute__((...))` correspondente |
| `__int64`, `__int32`... | `long long`, `int`... |
| `_stricmp`, `_strnicmp`, `_snprintf`, `_vsnprintf`, `_strdup`, `_strtoi64` | `strcasecmp`, `strncasecmp`, `snprintf`, `vsnprintf`, `strdup`, `strtoll` |
| `_alloca` | `alloca` |
| `_finite`, `_isnan` | `std::isfinite`, `std::isnan` |
| `_rotl`, `_rotr` | `__builtin_rotateleft32` / `std::rotl` |
| `InterlockedIncrement`/`Decrement`/`ExchangeAdd`/`CompareExchange[Pointer]`/`Exchange` | funções inline com `__atomic_*` e `__ATOMIC_SEQ_CST`. Mantêm a assinatura `volatile int32_t*` usada no código |
| `typeid(T).raw_name()` | macro `WOW_TYPE_RAW_NAME(T)`, que no Mac vira `typeid(T).name()` (a string só serve para log do alocador) |
| `MAX_PATH` | `PATH_MAX` ou 260, mantendo o tamanho dos buffers |

**Fazer:** remover os `#include <windows.h>` vestigiais dos headers de jogo listados no doc 01 e, nos headers da
camada de SO, trocar os membros `CRITICAL_SECTION`/`HANDLE` por tipos opacos da plataforma (ver doc 03).

**Não fazer:** um `windows.h` falso com a API inteira, como fazem projetos tipo winelib. Isso esconde dependências
reais e vira um segundo Wine.

## Correções de fonte que o clang exige

Em todos os casos o MSVC também aceita a versão corrigida, então o comportamento no Windows não muda:

- `Engine/Source/Base/CDataStore.h:70`: o parâmetro `size` sombreia `template <uint32_t size>`. Renomear o
  parâmetro.
- `Engine/Source/Event/Types.h:9,139,152`, `WoW/Source/Glue/Types.h:50` e `WoW/Source/WorldClient/Types.h:73`:
  um literal de 64 bits num `enum : uint32_t`. Usar `0xFFFFFFFF`.
- `WoW/Source/Battlenet/BSN/BSNEncodeInterface.cpp`: um `reinterpret_cast` que remove `const`.
- `raw_name()` em `Storm/H/stpl.h`, `Base/CDataAllocator.h` e ~10 `.cpp` (SoundEngine, Frame).
- `Lua 5.1 luaconf.h:562`: `__asm fld/fistp`. A guarda é `_M_IX86`, então não é ativado no Mac. Só confirmar.
- FMOD: `ASSERT` com `__asm int 3` em `fmod_octree.cpp:43`, `fmod_geometryi.cpp:17` e
  `fmod_geometry_mgr.cpp:64`. Trocar por `__builtin_trap()`.

## Entrada do programa

Hoje a cadeia é: `/ENTRY:start` → `start()` → `StormRtlInitialize()` → `WinMainCRTStartup` → `WinMain`, que
chama `ConvertThreadToFiber` e roda `main` numa fibra com pilha de 8 MB (`WoW/Source/Win64/WinMain.cpp:20-62`).

No Mac, o arquivo novo `WoW/Source/Mac/MacMain.mm` faz isto:

1. Chama `StormRtlInitialize()`.
2. Cria `NSApplication` e o delegate na thread principal. O AppKit exige UI e eventos na main thread.
3. Roda `CommonMain`. Há duas formas de fazer isso:
   - **(a) Recomendada:** `CommonMain` roda na main thread, e `OsInputGet` chama
     `[NSApp nextEventMatchingMask:untilDate:inMode:dequeue:]` e `sendEvent:`, imitando o
     `PeekMessage`/`DispatchMessage` original. Muda menos o fluxo e mantém janela e contexto GL na main thread.
     A pilha de 8 MB da fibra original é obtida com `-Wl,-stack_size,0x800000`.
   - **(b)** `CommonMain` roda numa `pthread` com pilha de 8 MB, e a main thread fica no `[NSApp run]`
     repassando eventos para a fila `OsQueue`. Isso exige sincronizar tudo que toca o AppKit.
4. Configura a FPU (equivalente ao `_control87`, ver doc 07).
