# Porte para macOS: etapas

✅ = feito · ⬜ = pendente · 🔶 = em andamento. Detalhes de cada etapa em [docs/08-roadmap.md](docs/08-roadmap.md).

## ✅ Estudo de viabilidade
✅ Métricas do código e experimento com o clang arm64 (docs 01–10)
✅ Decisões: só arm64, servidor privado com login Grunt, um código multiplataforma num fork

## 🔶 E0: Ambiente
✅ Xcode, CMake e Ninja instalados
✅ Fork próprio, branch `port-mac`, remote `upstream` (SatyPardus/wowdecomp)
✅ `Data/` do cliente 3.3.5a enUS localizado (`bin/Mac/Data` é um link simbólico, fora do git)
✅ CI no GitHub Actions
  ✅ macOS arm64: `check-clang.sh` + build do `.app`
  ✅ Windows x64 (MSVC)
  ⬜ Windows Win32: falha também na `main` original do upstream; job não bloqueante até investigar
🔶 Servidor privado local (o seu servidor; Warden desligado, conta sem `@`)
⬜ Referência visual: `Wow64.exe -opengl` no CrossOver/Whisky

## ✅ E1: Limpeza para o clang
✅ `port-mac/tools/check-clang.sh` (linha de base: 161/1046)
✅ `Base/Platform/Compat.h` (convenções de chamada, `__forceinline`, `__int64`, `_stricmp`, `Interlocked*`, `raw_name`...)
✅ `Base/Platform/Mac/Include`: `malloc.h`, `mbstring.h`, `winerror.h`, `xmmintrin.h`, `emmintrin.h`
✅ `Base/Platform/WinTypes.h` no lugar do `#include <windows.h>` do código comum
✅ Correções de fonte sem mudar corpo de função (enums de 64 bits, `CDataStore.h`, declarações `static`, `<cfloat>`)
✅ `Player_C.cpp` `GuildUpdateHandler`: cast por `uintptr_t` (selo `VERIFY` a refazer)
✅ `BSNEncodeInterface.cpp` fora do build Mac (só Battle.net)
✅ 100% dos arquivos passam no check (1114/1114)

## ✅ E2: Build e link com stubs
✅ CMake com plataforma macOS (arm64, `MACOSX_BUNDLE`, frameworks, pilha de 8 MB)
✅ Filtragem central das pastas `Win32/`, `Win64/`, `Mac/` e de `mac-excluded.txt`
✅ Terceiros em arm64 (zlib, bzip2, lzma, expat, FreeType, pcre, jpeg, lua)
⬜ xvidcore em arm64 (sem código-fonte no repositório; movido para o E7)
✅ sse2neon v1.8.0
✅ FMOD compilando com `PLATFORM_MAC` e saída NOSOUND
✅ `gen-mac-stubs.py` e `TODO_MAC()`; pastas `Mac/` com stubs para todo o `Win64/`
✅ `WoW/Source/Mac/MacMain.mm`
✅ `World of Warcraft.app` linka e abre até o primeiro `TODO_MAC()`

## ✅ E3: Fundação do SO
✅ `MacSync` (seções críticas recursivas, eventos, sleep, relógio)
✅ Blizzard::Lock, CCritSect
✅ Os: tempo, TimeManager, TLS, memória (páginas de 16 KB), caminhos, linha de comando, info do sistema, random, dlopen
✅ Storm: threads, SLock/SRWLock (corrigido para a ordem de memória do arm64), SLog, SReg (`Registry.ini`), SErr, SCmd, IOFileUnit
✅ BlizzardCore: threads e pools, TLS, tempo, arquivos (`\` → `/`, busca sem distinção de caixa), log, processo, streaming local
✅ FMOD: camada de SO POSIX
✅ `OsGetExePath` aponta para a pasta que contém o `.app`
✅ `port-mac/tests/DataTest`: 11.054 arquivos de MPQ, 7 DBCs e 10.307 BLPs, 0 falhas
✅ O cliente abre as 18 MPQs e para em "dispositivo de exibição" (começo do E4)

## 🔶 E4: Janela, OpenGL e input
✅ Funções WGL/Win32 do `CGxDeviceOpenGl.cpp` movidas sem mudança para `CGxDeviceOpenGL/Windows/` (122 funções conferidas por script)
✅ `CGxDeviceOpenGl` no Mac: contexto legado 2.1 (`NSOpenGLContext`), ponteiros GL via `dlsym`, swap interval, `flushBuffer`
✅ FBO no lugar de pbuffer
✅ D3D pedido pelo jogo vira o device OpenGL no Mac (sem mudar `GxDefaultApi()`)
✅ `CGxDevice/Mac`: adaptador genérico e modos da tela principal
✅ `Os/Mac`: `NSWindow` + `WowGameView`; `OsInputGet` drena eventos do Cocoa
✅ Input
  ✅ Teclado: tabela `kVK_*` → `KEY_*`, modificadores esquerdo/direito, repetição
  ✅ Mouse: 5 botões, posição, scroll, modo relativo
  ✅ Texto digitado (`insertText:`, teclas mortas)
✅ `GxTest` (device OpenGL, clear, present, leitura do back buffer) e `WindowTest` (27 eventos sintetizados)
✅ Correções de arm64 achadas no caminho: `CRITICAL_SECTION` desalinhada (`#pragma pack`), pilha das threads do FMOD, `INT32` de 64 bits na libjpeg, `fopen` com `\`
✅ Thread de rede sobe (`poll()`), Warden stub do Win64 usado no Mac
✅ **Tela de login renderizada** (screenshot do próprio jogo, 2560x1440)
⬜ Corrigir o modelo 3D da tela de login (aparece recortado num retângulo no centro)
⬜ Conferir à mão: digitar usuário e senha, clicar nos botões, redimensionar a janela
⬜ Vsync (swap interval 1 deu ~92 fps numa tela de 60 Hz)
⬜ Patches customizados `patch-6`/`patch-7` alteram o GlueXML e falham a assinatura (rodar com `WOWDECOMP_DISABLE_SIGNATURE_CHECKS` ou sem eles)

## ⬜ E5: Rede e login
⬜ `OsTcp` sobre kqueue
⬜ `WowConnectionNet` e `WowConnection` (errno, `O_NONBLOCK`)
⬜ `getaddrinfo` no lugar de `gethostbyname`
⬜ `IOsVersionChecksum` para Mach-O; `OsGetOsId` configurável
⬜ Stubs definitivos: Battle.net, Warden, BotCheck, ScanDLL, Survey, Patcher
⬜ Mouse-look básico
⬜ Login → reinos → criar personagem → entrar no mundo → andar e conversar

## ⬜ E6: Áudio
⬜ `fmod_output_coreaudio.cpp` (AudioUnit) e registro como saída padrão
⬜ Lista de dispositivos (`FMOD_OS_CheckDriverList`)
⬜ Testes: efeitos 3D, música em streaming, reverb, troca de dispositivo

## ⬜ E7: Jogabilidade completa
⬜ Mouse-look sem aceleração; cursor recapturado no alt-tab
⬜ Fullscreen como janela sem borda; resize, minimizar, foco, vários monitores, Retina
⬜ Atalhos (Cmd+Q, Cmd+Tab, Cmd como Ctrl opcional)
⬜ Clipboard, abrir URL, `NSAlert` (hoje `OsGuiMessageBox` só escreve no stderr)
⬜ Crash handler (`sigaction` + `backtrace`) e `SErrCatchUnhandledExceptions`
⬜ HTTP (`URL.cpp`, `OsURLDownload`)
⬜ Filmes (xvidcore arm64 + XvidDecoder)
⬜ Gamma (opcional)

## ⬜ E8: Empacotamento e estabilização
⬜ `.app` com ícone e assinatura ad-hoc
⬜ Layout de pastas (`Data/`, `WTF/`, `Interface/AddOns`, `Cache/`, `Screenshots/`) ao lado do `.app`
⬜ Sessão de 4 h; desempenho em Dalaran, Wintergrasp e ICC
⬜ Teclados ABNT2 e US, teclas mortas, Option
⬜ README de build e uso para Mac

## ⬜ E9 (opcional): backend Metal
