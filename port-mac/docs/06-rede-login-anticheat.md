# 06: Rede, login e anti-cheat

## Rede

Todo o código Windows de rede está em 4 arquivos `Win64/`, ~2 mil linhas no total.

| Arquivo | Linhas | Modelo | No Mac |
|---|---:|---|---|
| `Engine/Source/Os/Win64/OsTcp.cpp` | 994 | **IOCP + overlapped**: `CreateIoCompletionPort` (:837), `GetQueuedCompletionStatus` (:904, :957), `WSARecv`/`WSASend` (:301, :328), `AcceptEx` (:419). Bombeado por `OsNetPump` (`Event/EvtSched.cpp:79,585`) | **reescrever no modelo de prontidão** com kqueue e sockets não bloqueantes. A API exposta (`OsTcp*`, callbacks de conclusão) pode continuar igual; só a implementação interna muda de "conclusão" para "prontidão + `recv`/`send` manual". 1 a 2 semanas |
| `WoW/Source/WowServices/Win64/WowConnectionNet.cpp` | 188 | `WSAEventSelect` + `WaitForMultipleObjects` (limitado a `MAXIMUM_WAIT_OBJECTS`), `WSAEnumNetworkEvents`, um evento de wakeup | kqueue: `EVFILT_READ`/`EVFILT_WRITE`, wakeup por `EVFILT_USER` ou pipe. 2 a 3 dias |
| `WoW/Source/WowServices/Win64/WowConnection.cpp` | 736 | BSD sockets + `WSAGetLastError`, `ioctlsocket(FIONBIO)`, `closesocket` | troca mecânica: `errno`/`EAGAIN`, `fcntl(O_NONBLOCK)`, `close`. 1 dia |
| `Engine/Source/ComSat/Win64/ComSatNetworkSocketPosix.cpp` | 68 | UDP, `WSAIoctl(SIO_UDP_CONNRESET)` | remover o ioctl e trocar as chamadas. Horas |

Outros pontos de rede:

- `BattlenetLogin.cpp:16` usa `gethostbyname`/`inet_addr`, que são portáveis. Vale trocar por `getaddrinfo` para
  IPv6.
- `NetClient.cpp:83` usa `InterlockedExchangeAdd`, que o header de compatibilidade resolve.

A serialização de pacotes é feita campo a campo via `CDataStore`, então não depende do layout de structs. x86 e
arm64 são little-endian. Há `#pragma pack(1)` em `WowConnectionNet.h:14` e `ComSatNetwork.h:57`, e o clang
aceita igual.

## Login

O 3.3.5a tem dois caminhos, escolhidos em `ClientServices.cpp:835-847`.

> **Decisão (2026-10-19):** o porte será usado só com servidor privado, com usuário sem `@`. Só o caminho 1
> (Grunt/SRP6) precisa funcionar. O caminho 2 (Battle.net) fica como stub e não está no escopo.

### 1. Grunt / SRP6 (login clássico, usado por TrinityCore, AzerothCore etc.)

- `WowServices/Grunt.cpp`, `GruntLogin.cpp` e `SRP6_Client.cpp` são **C++ puro**, assim como PIN, matriz e token
  (`Grunt.cpp:597-661`).
- **Funciona no Mac sem mudanças**, além da rede.
- **Identificação de SO.** `GruntLogin::GetLogonMethod` (`GruntLogin.cpp:72`) envia `OsGetOsId()`, que hoje
  retorna `0x57696E` (`'Win'`, `Os/Win64/System.cpp:304`). O cliente Mac original enviava `'OSX'`. Pontos a
  considerar:
  - TrinityCore e AzerothCore usam o campo `os` para escolher **qual hash de executável** validar no
    `AUTH_LOGON_PROOF` (versionproof). Com `'OSX'`, um servidor com validação estrita esperaria o hash do
    cliente Mac oficial.
  - O próprio cliente olha esse campo na resposta (`Grunt.cpp:1608`, `notMac = os != 'MacP'...`).
  - **Recomendação:** criar um cvar ou opção de build (`WOWDECOMP_REPORT_OS`), com padrão **`'Win'`**, para o
    servidor tratar o porte como o build Windows desta árvore. Testar `'OSX'` como opção.

### 2. Battle.net (contas com `@`)

- `Battlenet/Tumor/CreepTendril.cpp:43-46` → `LoadApplicationBundle("Battle.net"...)` →
  `Battlenet/Win64/Battlenet.cpp:22-55`: faz `LoadLibraryW("Battle.net.dll")` e `GetProcAddress` pelos ordinais
  1 a 4.
- **É um binário proprietário da Blizzard para Windows, e não há como carregá-lo no Mac.** O cliente Mac original
  usava `Battle.net.dylib`, que não está disponível.
- **Ação:** stub que retorna "indisponível". Servidores privados não usam esse caminho. RealID e BattlenetUI
  dependem dele.

## Checksum do executável (versionproof)

`WowSvcs/WowSvcsClient/ChecksumExecutables.cpp` calcula um HMAC-SHA1 de três itens:

1. o próprio executável, via `IOsVersionChecksum`, que faz parse de cabeçalhos PE (`Os/Win64/OsVersionHash.cpp:374-469`);
2. `DivxDecoder.dll`;
3. `unicows.dll`.

O resultado vai para o servidor no login.

- O build x64 desta árvore **já produz um hash diferente do `Wow.exe` original**, porque é outro binário. Na
  prática, os servidores que aceitam o build Windows atual não validam esse hash ou estão configurados para
  aceitá-lo. O porte Mac fica na mesma situação.
- No Mac, `IOsVersionChecksum` precisa de uma implementação para Mach-O ou de um modo explícito. Opções:
  - **(a) Recomendado:** `IOsVersionChecksum` faz hash das seções `__TEXT`/`__DATA` do Mach-O, ou do arquivo
    inteiro. Determinístico, sem dados forjados.
  - **(b)** Uma opção de build para enviar o mesmo checksum que o build Windows enviaria. Isso só faz sentido se
    um servidor que você controla exigir.
- `OsFindTextSectionBounds`/`OsValidateFunctionPointer` (`Os/Win64/System.cpp:333-356`) são usados para validar
  ponteiros de função de cvars e comandos (`ConsoleVar.cpp:392`, `ConsoleClient.cpp:963`). No Mac:
  `getsectiondata(&_mh_execute_header, "__TEXT", "__text", &size)`.

## Anti-cheat e código baixado

| Componente | Estado no x64 | Mac |
|---|---|---|
| **Warden** | `Game/GameClient/Win64/WardenClient.cpp` (114 linhas) já é stub: consome `SMSG_WARDEN_DATA` e não responde. O Win32 (769 linhas) usa `BLLLoader`, um loader PE próprio que executa código x86-32 enviado pelo servidor | copiar o stub. **O servidor precisa estar com Warden desligado**, mesma exigência do build x64 atual |
| `BLLLoader` | `Os/Win64/BLLLoader.cpp` vazio (5 linhas) | vazio |
| `BotCheck` | `WoW/Source/Win64/BotCheck.cpp`: `__try` + teste do byte `0xE9` (hook) em `OsValidateFunctionPointer`, scans em stub | stub |
| `ScanDLLGlue` (Scan.dll) | carrega DLL baixada (`Glue/ScanDLLGlue.cpp:44`, `CheckExecutableSignature.cpp:42`) | stub "sem hack detectado" |
| `SurveyDownloadGlue` (`Cache\Survey.dll`) | `Glue/SurveyDownloadGlue.cpp:186` | stub |
| Patcher (`PatchDownloadGlue`, `ShellExecuteA("runas")`) | `Glue/Win64/PatchDownloadGlue.cpp:20` | stub. Não há patches para 3.3.5a |

Nenhum desses pontos bloqueia o porte, porque o build x64 do Windows já convive com as mesmas limitações.
