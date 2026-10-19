# 03: Camada de plataforma (Os / Storm / BlizzardCore)

## Arquitetura existente

O cliente original da Blizzard era multiplataforma (Windows e Mac), e a decompilação preservou essa
arquitetura. São três níveis de abstração, e cada um tem a implementação Windows em `Win32/` e `Win64/`:

| Nível | Interface | Implementação x64 | Tamanho |
|---|---|---|---|
| `Os*` | `Engine/Source/Os/*.h` | `Engine/Source/Os/Win64/` | 6,8 mil linhas, ~246 funções |
| Storm (`S*`) | `Engine/Source/Storm/H`, `Storm/Source/*.h` | `Storm/Source/Win64/` | 4,3 mil linhas, ~202 funções |
| `Blizzard::*` (BlizzardCore) | `BlizzardCore/Source/System/*`, `Packages/*` | `.../System/*/Win64`, `.../Packages/{Streaming,iounit}/Win64` | ~6,5 mil linhas |
| WowServices | `WoW/Source/WowServices/*.h` | `WowServices/Win64/` (rede) | ~1 mil linhas |

Os módulos de jogo (WoW/*) passam por essas camadas e quase não chamam a API Win32 diretamente.

Há resquícios do porte Mac original que confirmam o desenho:

- `Script_IsMacClient` (`Glue/GlueScriptEvents_Scripts.cpp:801`) e o teste `'MacP'` em
  `WowServices/Grunt.cpp:1608`.
- O enum `GxApi_GLL` (`Gx/Types.h`), que era a camada GL do cliente Mac.
- O nome `ComSat/Win64/ComSatNetworkSocketPosix.cpp`.
- `Blizzard::String::MakeUnivPath`/`MakeBackslashPath` e `File::FromNativeName`, a camada de caminhos
  "universais".

Não há nenhum código Mac utilizável: nenhum `#ifdef __APPLE__` no código próprio.

## Vazamentos de Windows

1. **Headers públicos com tipos Win32.**
   - `Storm/H/SCritSect.h` (`CRITICAL_SECTION m_critsect`), `Storm/Source/SThread.h`, `SLock.h`, `SMsg.h` e `SLog.h`.
   - `BlizzardCore/.../Thread/Thread.h` (`HANDLE m_handle`), `Lock/Lock.h`, `File/File.h` e `Time/Time.h`.

   Por transitividade, ~43 arquivos fora de `Win*/` acabam incluindo `windows.h`.

   **Correção:** pôr os tipos de plataforma em headers `Platform/` com membros opacos do mesmo tamanho, ou com
   tamanho próprio no Mac. Isso é aceitável porque a validação de layout é feita no alvo Win32.

2. **Assinaturas com tipos Win32.** `OsWindowProc(void*, uint32_t msg, uintptr_t, intptr_t)` e
   `OsGetDefaultWindowRect(tagRECT*)`. No Mac: `struct OsRect` próprio, e um `OsWindowProc` que não sai da camada
   Mac.

3. **`Interlocked*`**: ~115 usos (`Event/EvtSched.cpp:280-488`, `Base/CDataRecycler.cpp`, `Net`,
   `WowServices`). Tratar no header de compatibilidade (doc 02).

4. **`OVERLAPPED`** na API de I/O assíncrona da BlizzardCore (Mopaq, iounit, Streaming, File e Log): 17 arquivos.
   Definir `Blizzard::File::Overlapped` com implementação POSIX. Para arquivos locais (MPQ) basta `pread` síncrono
   numa thread de I/O; dá para usar `dispatch_io` mais tarde.

## Mapeamento por família de API

As contagens somam Win32 e Win64. Para o alvo Mac conta mais ou menos a metade.

| Família | Uso hoje | Implementação no Mac | Dificuldade |
|---|---|---|---|
| Arquivos (CreateFile, Read/WriteFile, FindFirst/Next, CreateDirectory: ~200 ocorrências) | `Blizzard::File` (`File/Win64/*.cpp`, 1,2 mil linhas) | `open`/`pread`/`pwrite`/`fstat`/`opendir`/`mkdir` | baixa |
| Caminhos com `\` (746 literais em 366 arquivos) | o hash de MPQ normaliza (`SStr.cpp:795`) | converter `\`→`/` só em `File::ToNative`, sem mexer nos literais | baixa |
| Case-insensitive (arquivos soltos: AddOns, WTF, Interface) | `BuildFileSystemHash` (`SFile2-Core.cpp:960+`) | APFS padrão é case-insensitive. Em volume case-sensitive, procurar ignorando caixa no `ToNative` | baixa/média |
| Threads (CreateThread, prioridade, Sleep) | `SThread`, `Blizzard::Thread` | `pthread_create`, QoS classes, `nanosleep` | baixa |
| CRITICAL_SECTION (~270) | `SCritSect`, `Blizzard::Lock` | `pthread_mutex` recursivo ou `os_unfair_lock` | baixa |
| Eventos/espera (CreateEvent, SetEvent, WaitFor*: ~220) | `Blizzard::Lock::Event`, `SEvent` | `pthread_cond` + flag. `WaitForMultipleObjects` só aparece na rede | média |
| TLS (16) | `OsTls*`, `Blizzard::Thread::TLS` | `pthread_key_*` ou `thread_local` | baixa |
| Fibras (8, só em WinMain) | `ConvertThreadToFiber` | eliminar: thread principal com pilha de 8 MB | baixa |
| Tempo (QPC, GetTickCount, GetLocalTime: ~100) | `OsGetAsyncTimeMs`, `TimeManager` | `mach_absolute_time`/`clock_gettime_nsec_np`, `localtime_r` | baixa |
| Memória (VirtualAlloc, Heap*: ~50) | SMem, EmergencyMem | `mmap`/`mprotect`, `malloc` | baixa |
| Registro (SReg: 16) | `Storm/Source/Win64/SReg.cpp` | `CFPreferences` ou um arquivo `.plist`/`.ini` em `~/Library/Preferences` | baixa |
| Clipboard (8) | `OsClipboard` | `NSPasteboard` | baixa |
| IME (imm32, ~70) | `OsIME` | `NSTextInputClient` na view do jogo | **alta** (necessário para chinês, coreano e japonês; pode ser adiado) |
| Teclado (VK_*: 222; GetAsyncKeyState etc.: 28) | `Os/Win64/Input.cpp`: `ConvertKeyCode:354`, `ConvertKeyToVirtualKey:543` | tabela `kVK_*` (Carbon `Events.h`) → `KEY_*` e `NSEvent.modifierFlags` | média |
| Mouse, cursor, captura (~72) | `Input.cpp`, `OsCursor*` | `NSCursor`, `CGAssociateMouseAndMouseCursorPosition` + deltas (mouse-look) | **média/alta**: o mouse-look do WoW precisa de captura relativa sem aceleração |
| Mensagens de janela (WM_*: 349) | `OsWindowProc` (`Input.cpp:1326`) | `NSWindowDelegate` + `NSView` (resize, foco, minimizar, fullscreen) | média |
| Joystick (DirectInput8) | `Os/Win64/Joystick.cpp` | `GameController.framework` ou stub | baixa (stub) |
| MessageBox (4) | `OsGui*` (`Gui.cpp:182`), `SErr` | `NSAlert` | baixa |
| Abrir URL (ShellExecute) | `OsLaunchURL` (`System.cpp:496`) | `NSWorkspace openURL` | baixa |
| HTTP (wininet: 16) | `OsURLDownload` | `NSURLSession` ou libcurl | baixa/média |
| Aleatório seguro (CryptGenRandom) | `OsSecureRandom` | `SecRandomCopyBytes`/`arc4random_buf` | baixa |
| Versão do executável (version.dll) | `Blizzard::Version` | `CFBundleGetValueForInfoDictionaryKey` ou constante | baixa |
| Info de hardware (`__cpuid`, ~23) | `Hardware/Win64`, `Os/Win64/System.cpp` | `sysctlbyname("machdep.cpu.*" / "hw.*")` | baixa |
| Exceções e crash (SEH: 18 pares; SetUnhandledExceptionFilter; dbghelp/MiniDump) | `SErr`, `MachineState` | `sigaction` (SIGSEGV/SIGBUS/SIGILL) + `backtrace()`, crash reporter do macOS. Sem minidump | média |
| Loader de DLL (LoadLibrary/GetProcAddress: ~180) | `OsLoadLibrary`, Battlenet, Survey, ScanDLL, OpenAL loader | `dlopen`/`dlsym` onde fizer sentido. Stub nos módulos Windows (doc 06) | baixa |
| Parse de PE (`OsFindTextSectionBounds`, `IOsVersionChecksum`, `MachineState`) | `Os/Win64/System.cpp:333-356`, `OsVersionHash.cpp:374-469` | Mach-O `getsectiondata(&_mh_execute_header, "__TEXT", "__text")`. Ver doc 06 sobre o checksum | média |
| Rede (Winsock + IOCP: ~110) | `OsTcp`, `WowConnectionNet` | kqueue (doc 06) | média/alta |

## Janela e loop de eventos

Fluxo atual:

1. `CommonMain` (`WoW/Source/Client.cpp:1830`) chama `EventDoMessageLoop` (`Event/Event.cpp:42`).
2. A cada iteração, `IEvtInputProcess` chama `OsInputGet` (`Event/EvtInput.cpp:498`).
3. `OsInputGet` faz `PeekMessage`/`DispatchMessage` (`Os/Win64/Input.cpp:1207-1230`).
4. O `WndProc` traduz para `OsQueuePut` (`CKeyEvent`/`CMouseEvent`).

Daí para cima tudo é portável.

**No Mac**, implementar `Os/Mac/Input.mm`:

- `OsInputGet` drena `NSApp nextEventMatchingMask` sem bloquear.
- Uma `NSView` (`WowGameView`) traduz `keyDown`/`keyUp`/`flagsChanged`/`mouse*`/`scrollWheel` para
  `OsQueuePut`, reusando as mesmas constantes `KEY_*`.
- Repare que a janela hoje é criada **pelo backend Gx** (`CGxDeviceOpenGl.cpp:1637-1709`), e não pelo Os. No Mac,
  o ideal é que o Gx peça a janela e a view ao `Os/Mac` (`OsCreateGameWindow`) e anexe o contexto GL a ela.

Teclas e comportamentos específicos do Mac:

- Cmd ↔ Ctrl: o cliente Mac original tratava Cmd como Ctrl nos atalhos de sistema (Cmd+Q, Cmd+Tab).
- Fullscreen: usar "janela sem borda no tamanho da tela" (`NSWindowStyleMaskBorderless` + nível
  `NSMainMenuWindowLevel+1`) em vez de trocar o modo de vídeo. `ChangeDisplaySettingsEx` não tem equivalente
  prático no macOS moderno.
- Retina: renderizar em pixels físicos (`wantsBestResolutionOpenGLSurface`) e informar ao jogo o tamanho em
  pixels, ou renderizar em pontos e deixar o compositor escalar. O segundo é mais simples e roda melhor.

## Arquivos a criar (estimativa)

| Pasta nova | Baseada em | Arquivos | Funções aprox. |
|---|---|---:|---:|
| `Engine/Source/Os/Mac/` | `Os/Win64` (21 arquivos) | ~20 | ~240 |
| `Engine/Source/Storm/Source/Mac/` | `Storm/Source/Win64` (11) | ~11 | ~200 |
| `Engine/Source/BlizzardCore/Source/System/*/Mac/` e `Packages/*/Mac/` | Win64 equivalentes | ~20 | ~170 |
| `Engine/Source/Gx/CGxDevice/Mac/` + `CGxDeviceOpenGL` (CGL) | ver doc 04 | ~4 | ~60 |
| `WoW/Source/WowServices/Mac/` | `WowServices/Win64` (3) | 3 | ~40 |
| `WoW/Source/Mac/` (MacMain, BotCheck stub) | `WoW/Source/Win64` | 2 | ~10 |
| `WoW/Source/Game/GameClient/Mac/` (Warden stub) | Win64 (114 linhas) | 1 | ~10 |
| ComSat, Battlenet, Glue (sockets, stubs) | Win64 | ~4 | ~20 |
| **Total** | | **~65 a 70** | **~750** |

Cerca de 60% dessas funções são invólucros de poucas linhas.
