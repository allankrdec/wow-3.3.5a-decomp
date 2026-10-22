# 08: Plano de etapas

Decisões que valem para todo o plano:

- **Alvo:** macOS **arm64 nativo** (Apple Silicon).
- **Uso:** servidor privado, login Grunt/SRP6, usuário sem `@`.
- **Gráficos:** backend OpenGL existente com contexto legado 2.1. O Metal fica para depois (etapa 9).

## Princípios

1. **Não mudar o comportamento do código validado.** As funções com `// OFFSET:` são comparadas com o binário
   original. O código Mac fica em pastas `Mac/` paralelas a `Win32/` e `Win64/`. Mudanças no código comum só
   entram se forem neutras para o MSVC e devem ir em commits ou PRs separados, para facilitar o upstream.
2. **Compilar, depois linkar, depois rodar.** Stubs vêm antes das implementações. Cada stub registra o primeiro
   uso com `TODO_MAC()`, o que mostra o próximo trabalho.
3. **Validar por camadas com executáveis pequenos** antes do cliente inteiro: ler MPQ, ler DBC, abrir janela e
   desenhar. Um bug achado num teste de 50 linhas custa bem menos que um achado dentro do jogo.
4. **Cada etapa termina com algo demonstrável.**

## Visão geral

```
E0 Ambiente ──► E1 Limpeza clang ──► E2 Build + stubs ──► E3 Fundação SO ──► E4 Janela/GL/Input ──► E5 Rede/Login ──► E7 Jogabilidade ──► E8 Empacotamento
                                                                  │                                            ▲
                                                                  └────────► E6 Áudio (em paralelo) ───────────┘
                                                                                                   E9 Metal (opcional, depois de E7)
```

| Etapa | Entregável | Estimativa (1 dev) |
|---|---|---|
| E0 | Servidor privado local rodando; ambiente pronto | 2 a 3 dias |
| E1 | Código comum compila no clang arm64 (`-fsyntax-only`) | 1 a 2 semanas |
| E2 | `.app` compila e **linka** no arm64 | 2 a 3 semanas |
| E3 | `MpqTest`/`DbcTest` leem os dados do jogo no Mac | 2 a 3 semanas |
| E4 | **Tela de login renderizada** e teclado funcionando | 3 a 4 semanas |
| E5 | **Entrar no mundo** e andar | 2 a 3 semanas |
| E6 | Som e música (pode correr em paralelo com E4/E5) | 2,5 a 4 semanas |
| E7 | Jogável de verdade: mouse-look, fullscreen, clipboard, crash log, filmes | 2 a 3 semanas |
| E8 | `.app` empacotado, estável em sessão longa | 1 a 2 semanas |
| **Total** | **cliente jogável no Mac** | **~3,5 a 5,5 meses** (2 a 3 meses com 2 ou 3 pessoas) |
| E9 | Backend Metal (opcional) | 4 a 8 meses |

---

## E0: Ambiente (2 a 3 dias)

**Objetivo:** ter tudo para testar sem depender de terceiros.

- [ ] Xcode / Command Line Tools e CMake ≥ 3.24 (Ninja recomendado: `brew install ninja`).
- [ ] Cópia do `Data/` do cliente 3.3.5a (12340) num diretório fixo, por exemplo `~/wow335/Data`.
- [ ] **Servidor privado local**: AzerothCore ou TrinityCore 3.3.5 via Docker no próprio Mac. Configuração:
  - [ ] Warden desligado (AzerothCore: `Warden.Enabled = 0`);
  - [ ] sem validação estrita de versão/hash do executável;
  - [ ] uma conta de teste sem `@`.
- [ ] Confirmar que o servidor aceita o cliente Windows desta árvore. Se não houver Windows à mão, testar com
      `Wow64.exe` no CrossOver/Whisky. Isso também serve de referência visual com `-opengl`.
- [ ] Fork próprio; branch de trabalho `port-mac`, sem commits direto na `main` (doc 10).
- [ ] CI no GitHub Actions: build Windows (x64 e Win32) e macOS arm64 a cada push (doc 10).

**Pronto quando:** o servidor está no ar e uma conta de teste existe.

## E1: Limpeza para o clang (1 a 2 semanas)

**Objetivo:** o código comum compila no Apple clang arm64 **sem mudar nada no Windows**.

**Status (2026-10-22):** 1037 de 1042 arquivos passam no `check-clang.sh` (linha de base: 161 de 1046).
Os 5 que faltam usam SSE (`M2Model`, `M2Scene`, `CharacterComponent`, `SpiritFrame`, `SpiritPitch`) e ficam
para o sse2neon do E2.
- `Player_C.cpp:682` (ponteiro → `uint32_t`) foi corrigido no corpo de `GuildUpdateHandler`; o selo
  `VERIFY` dessa função precisa ser refeito.
- `BSNEncodeInterface.cpp` (só Battle.net) saiu do build Mac em vez de mudar ~30 corpos com selo `VERIFY`. O
  que linkar com ele vira stub no E2.

Decisões tomadas nesta etapa:
- `raw_name` virou `#define raw_name name` no `Compat.h`, sem macro nova nos ~120 pontos de uso.
- `Platform/WinTypes.h` substitui o `#include <windows.h>` do código comum. No Windows ele inclui o
  `windows.h`; no Mac define só os tipos opacos.
- CRT que falta no SDK do macOS (`malloc.h`, `mbstring.h`, `winerror.h`) fica em
  `Base/Platform/Mac/Include`.
- Arquivos só do Windows em pastas comuns vão para `port-mac/tools/mac-excluded.txt`.

- [ ] `Engine/Source/Base/Platform/Compat.h` (doc 02):
  - convenções de chamada vazias;
  - `__forceinline` e `__int64`;
  - `_stricmp` e família;
  - `_alloca`, `_finite`, `_isnan`, `_rotl`;
  - `Interlocked*` sobre `__atomic_*`;
  - `MAX_PATH`;
  - `WOW_TYPE_RAW_NAME(T)`.
- [ ] `raw_name()` → macro (`Storm/H/stpl.h`, `Base/CDataAllocator.h`, ~10 `.cpp`).
- [ ] Enums com `0xFFFFFFFFFFFFFFFF` em base de 32 bits: `Event/Types.h:9,139,152`, `Glue/Types.h:50`,
      `WorldClient/Types.h:73`.
- [ ] `Base/CDataStore.h:70`: renomear o parâmetro sombreado.
- [ ] `Battlenet/BSN/BSNEncodeInterface.cpp`: corrigir o cast que remove `const`.
- [ ] Remover `#include <windows.h>` vestigiais dos headers de jogo: `Object.h`, `Object_C.h`, `Unit_C.h`,
      `Player_C.h`, `SpellCast.h`, `Game/Types.h`, `Glue/Types.h`, `PlayerName.h`, `NetInternal.h`, `GameUI.h`.
- [ ] Headers da camada de SO com tipos opacos no lugar de `CRITICAL_SECTION`/`HANDLE`: `SCritSect.h`,
      `SThread.h`, `SLock.h`, `Thread.h`, `Lock.h`, `File.h`, `Time.h`.
- [ ] Tipo `OVERLAPPED` próprio na BlizzardCore (17 arquivos de Mopaq, iounit e Streaming).
- [ ] Script `port-mac/tools/check-clang.sh` que roda o `-fsyntax-only` (o do doc 01) sem shim.

**Pronto quando:** 100% dos `.cpp` comuns passam no `check-clang.sh` e o build Windows x64/Win32 continua
compilando e com o mesmo comportamento.

## E2: Build e link com stubs (2 a 3 semanas)

**Objetivo:** gerar `World of Warcraft.app` arm64 que linka, mesmo que pare no primeiro `TODO_MAC()`.

**Status (2026-10-22): concluído.** `cmake -G Ninja` + `ninja` gera `bin/Mac/World of Warcraft.app` (build limpo
em ~1min20s); o `check-clang.sh` passa 1114/1114 (o E1 também está fechado com o sse2neon). Ao abrir, o app
registra os primeiros `TODO_MAC()` (SCritSect, RMutex, Time, Thread, SLog, Storm...) e fica em
`Blizzard::Process::Sleep`, porque `Atomic::Increment` ainda é stub. Diferenças em relação ao plano:
- a filtragem de pastas por plataforma é feita uma vez no `CMakeLists.txt` raiz, não em cada módulo;
- os stubs vêm de `port-mac/tools/gen-mac-stubs.py` e não têm a tag `OFFSET:` (usam `// Win64: 0x...`);
- o FMOD já tem a camada de SO POSIX de verdade (só a saída CoreAudio fica para o E6);
- o xvidcore não tem código-fonte no repositório (só a `.lib` x64), então os filmes ficam para o E7 e o
  DivxDecoder do Mac falha de propósito;
- o build Windows não foi testado depois das mudanças no CMake: falta a CI.

- [ ] CMake (doc 02):
  - [ ] trocar o `FATAL_ERROR` por `WOWDECOMP_PLATFORM_MAC`;
  - [ ] `WIN32`/`_WINDOWS` só no Windows;
  - [ ] filtro de pastas `Win32/` e `Win64/`;
  - [ ] `CMAKE_OSX_ARCHITECTURES=arm64`;
  - [ ] `-ffp-contract=off`;
  - [ ] alvo `MACOSX_BUNDLE` com `Info.plist`;
  - [ ] `enable_language(OBJCXX)`;
  - [ ] frameworks.
- [ ] Terceiros para arm64: zlib, bzip2, lzma, expat, FreeType, pcre e xvidcore (`--disable-assembly`).
      Excluir nvapi, lgLcd e DivxDecoder.
- [ ] sse2neon em `ThirdParty/` e incluído em M2Model, M2Scene, CharacterComponent e SPIRIT.
- [ ] Pastas `Mac/` com **stubs** para todas as funções de `Win64/`:
  - Os, Storm, BlizzardCore (System e Packages), Gx/CGxDevice e CGxDeviceOpenGL;
  - WowServices, Game/GameClient (Warden), WoW/Source (MacMain), ComSat, Battlenet e Glue.
- [ ] Excluir do build Mac: `CGxDeviceD3d*`, `Ui/UILCD/Windows`, `SteelseriesMouse/Win*`, `crash_log.cpp`.
- [ ] FMOD compilando com `PLATFORM_MAC` + arm64 (`fmod_settings.h`) e saída `NOSOUND`. O resto do áudio fica
      para E6.

**Pronto quando:** `cmake --build` gera o `.app`, e ao abrir aparece o log do primeiro `TODO_MAC()`.

## E3: Fundação do SO (2 a 3 semanas)

**Objetivo:** tudo que não é janela, input ou rede funcionando de verdade.

- [ ] Threads, mutex recursivo, eventos (`pthread_cond`), TLS, `Sleep` e prioridade.
- [ ] Tempo (`mach_absolute_time`, `localtime_r`, fuso horário).
- [ ] Memória (`mmap`/`mprotect`, page size de **16 KB** via `vm_page_size`), revisando `EmergencyMem` e SMem.
- [ ] Arquivos POSIX: `\` → `/` em `File::ToNative`, lookup ignorando caixa para arquivos soltos, `opendir`,
      `mkdir -p`.
- [ ] I/O assíncrona (`OVERLAPPED` próprio) sobre `pread` numa thread de I/O.
- [ ] `SReg` sobre arquivo `.plist`/`.ini`; `SLog`; `OutputDebugString` → `os_log`/stderr.
- [ ] Hardware: `sysctlbyname` no lugar de `__cpuid`; `OsGetProcessorId` continua `'x86'`.
- [ ] `MacMain.mm`: `StormRtlInitialize`, `feclearexcept`, pilha de 8 MB (`-Wl,-stack_size,0x800000`) e
      `CommonMain` na main thread.
- [ ] **Executáveis de teste** em `port-mac/tests/`:
  - `MpqTest`: abre as MPQs, lista e lê arquivos e confere o CRC;
  - `DbcTest`: carrega os DBCs usados pelo cliente (`WoW/Source/DB`);
  - `BlpTest`: decodifica BLPs.

**Pronto quando:** os três testes passam com o `Data/` real.

## E4: Janela, OpenGL e input (3 a 4 semanas)

**Objetivo:** ver a tela de login e digitar nela.

- [ ] `Os/Mac`: `NSApplication`, `NSWindow` e `WowGameView` (NSView). `OsInputGet` drena eventos com
      `nextEventMatchingMask` (doc 02, opção a).
- [ ] Gx: contexto legado 2.1 (`NSOpenGLContext`/CGL) na view, ponteiros GL via `dlsym`, swap interval e
      `flushBuffer`, FBO no lugar de pbuffer.
- [ ] `GxDefaultApi()` retorna `GxApi_OpenGl` no Mac.
- [ ] `CGxDevice/Mac`: lista de resoluções (`CGDisplayCopyAllDisplayModes`) e adaptador.
- [ ] Input:
  - teclado: tabela `kVK_*` → `KEY_*` e modificadores;
  - mouse: botões, posição e scroll;
  - texto digitado via `insertText:`.
- [ ] Teste intermediário `GxTest`: janela que limpa a tela, desenha um quad texturizado (BLP) e carrega um
      shader `arbvp1`/`arbfp1` de uma MPQ.

**Pronto quando:** a tela de login do GlueXML renderiza corretamente e aceita usuário e senha.

## E5: Rede e login (2 a 3 semanas)

**Objetivo:** entrar no mundo pelo servidor local.

- [ ] `OsTcp` sobre **kqueue** (doc 06): reescrever o IOCP no modelo de prontidão mantendo a API.
- [ ] `WowConnectionNet` (`WSAEventSelect` → kqueue) e `WowConnection` (errno, `O_NONBLOCK`).
- [ ] `getaddrinfo` no lugar de `gethostbyname`.
- [ ] `IOsVersionChecksum` para Mach-O; `OsGetOsId` padrão `'Win'`, configurável.
- [ ] Stubs definitivos: Battle.net DLL, Warden (copiar o stub do Win64), BotCheck, ScanDLL, Survey e Patcher.
- [ ] Mouse-look básico (captura relativa) para conseguir andar.

**Pronto quando:** login → lista de reinos → criar personagem → entrar no mundo → andar e conversar no chat.

## E6: Áudio (2,5 a 4 semanas, em paralelo com E4/E5)

**Objetivo:** som e música.

- [ ] `fmodex/mac/src/fmod_os_misc.cpp` (pthread, `dispatch_semaphore`, `mach_absolute_time`, `dlopen`).
- [ ] `fmod_os_output.cpp` (Mac) e `fmod_os_net.cpp` (stub).
- [ ] `fmod_output_coreaudio.cpp` (AudioUnit + mixer de software).
- [ ] Desligar CDDA, ASF, ACM, winamp e VST; `ASSERT int 3` → `__builtin_trap`.
- [ ] Testes: efeitos 3D, música em streaming, reverb, troca de dispositivo de saída.

**Pronto quando:** som 3D e música funcionam no mundo sem estalos.

## E7: Jogabilidade completa (2 a 3 semanas)

- [ ] Mouse-look sem aceleração; cursor escondido e recapturado em alt-tab.
- [ ] Fullscreen como janela sem borda; resize; minimizar; foco; múltiplos monitores; Retina.
- [ ] Atalhos: Cmd+Q, Cmd+Tab, Cmd como Ctrl opcional.
- [ ] Clipboard (`NSPasteboard`), abrir URL (`NSWorkspace`), `NSAlert`.
- [ ] Crash handler: `sigaction` + `backtrace` → `WowClient_crash.log`.
- [ ] Filmes (XvidDecoder estático).
- [ ] Gamma (opcional).

**Pronto quando:** dungeon e raid completos, com addons populares (DBM, Recount, Bartender) funcionando.

## E8: Empacotamento e estabilização (1 a 2 semanas)

- [ ] `.app` com ícone, `Info.plist`, assinatura ad-hoc (`codesign -s -`).
- [ ] Layout de pastas: `Data/`, `WTF/`, `Interface/AddOns`, `Cache/` e `Screenshots/` ao lado do `.app`, ou
      caminho configurável.
- [ ] Sessão de 4 h com troca de zonas e `/reload`; testes de desempenho em Dalaran, Wintergrasp e ICC.
- [ ] Teclados ABNT2 e US, teclas mortas, AltGr/Option.
- [ ] README de build e uso para Mac.

**Pronto quando:** alguém que não é o desenvolvedor consegue compilar, configurar e jogar seguindo o README.

## E9 (opcional): backend Metal

Ver doc 04, opção (b). Começar depois de E7, atrás da mesma interface `CGxDevice`. O OpenGL continua como
fallback até haver paridade visual.

## Fora do escopo (por decisão)

- Login Battle.net (contas com `@`) e RealID.
- Binários x86_64, Rosetta e universal binary.
- Warden ativo.
- Voice chat e IME CJK: só se houver demanda, depois de E8.
