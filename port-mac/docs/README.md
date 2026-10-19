# Porte do wowdecomp para macOS: estudo de viabilidade

Data: 2026-10-19. Base: commit `b1c71b8` (branch `main`).
Máquina de teste: macOS 26.6.2, Apple Silicon (arm64), Apple clang 17.0.0, CMake via Homebrew.

## Veredito

**O porte é viável.** É um projeto de médio porte, não uma reescrita. O código foi organizado como o cliente
multiplataforma original da Blizzard: quase tudo que depende do Windows está isolado em pastas `Win32/` e
`Win64/`, e a lógica de jogo (Ui, Object, Magic, WorldClient, DB, Frame, Lua) já é C++ portável.

Três números resumem o estudo:

| Medida | Valor |
|---|---|
| Código-fonte próprio (Engine + WoW + Tools, sem ThirdParty) | ~940 mil linhas, das quais ~270 mil são o FMOD Ex |
| Código específico de plataforma no alvo x64 (pastas `Win64/`) | ~21,6 mil linhas em ~66 arquivos `.cpp` (sem o FMOD) |
| Arquivos não-plataforma que **já compilam** com o clang do macOS arm64 (`-fsyntax-only`), depois de um shim mínimo e 6 correções triviais | **1000 de 1046 (95,6%)** |

Os 46 que ainda falham pedem só itens de um header de compatibilidade (`OVERLAPPED`, `Interlocked*`,
`_stricmp`, `_finite`...) ou troca de SSE por NEON. Detalhes em [01-metricas-e-experimento-clang.md](01-metricas-e-experimento-clang.md).

## Os três trabalhos grandes

1. **Camada de plataforma** (Os, Storm, BlizzardCore, WowServices): reimplementar ~650 a 760 funções em
   POSIX/Cocoa. Cerca de 60% é mapeamento direto (pthread, mmap, clock_gettime). O resto, que é a parte difícil, é
   janela, input/IME, rede IOCP e crash handling.
2. **Gráficos:** o repositório **já tem um backend OpenGL completo** (`CGxDeviceOpenGl`, ~6,6 mil linhas, sem
   stubs). Ele usa GL legado e programas ARB assembly (`arbvp1`/`arbfp1`), que o contexto legado 2.1 do macOS ainda
   suporta, inclusive no Apple Silicon. Basta trocar as ~1.300 linhas de WGL por CGL/NSOpenGL.
3. **Áudio:** o FMOD Ex está em código-fonte, mas sem saída CoreAudio. É preciso escrever `fmod_output_coreaudio`
   e a camada de SO do FMOD.

## Estimativa

| Marco | Esforço (1 dev experiente) |
|---|---|
| M0: build com CMake e clang, linkando com stubs | 2 a 3 semanas |
| M1: tela de login renderizada (janela, GL, arquivos, MPQ) | +4 a 6 semanas |
| M2: entrar no mundo e jogar (rede, input completo, áudio) | +5 a 8 semanas |
| M3: polimento (IME, clipboard, crash, filmes, voz, .app, assinatura) | +3 a 5 semanas |
| **Total até um cliente jogável no macOS** | **~3,5 a 5,5 meses** |
| Opcional: backend Metal nativo (substitui o GL deprecated) | +4 a 8 meses |

Com 2 ou 3 pessoas em paralelo (plataforma / Gx / áudio), o total cai para ~2 a 3 meses.

## Decisões tomadas

- **Arquitetura:** só arm64 nativo (Apple Silicon). Sem x86_64/Rosetta e sem universal binary (doc 07).
- **Servidor:** só servidor privado, com usuário sem `@`, usando login Grunt/SRP6 (doc 06).
- **Repositório:** um único código multiplataforma (Windows + Mac) num fork, com CI para as duas plataformas
  (doc 10).

## Bloqueadores

Não há bloqueador técnico absoluto. Pontos que precisam de decisão:

- **Login: resolvido.** O alvo é servidor privado, com usuário sem `@`, então o cliente usa o login clássico
  (Grunt/SRP6), que é C++ puro e funciona no Mac. O caminho Battle.net (contas com `@`), que carrega uma DLL
  proprietária, vira apenas um stub (doc 06).
- **Warden** já está em stub no x64, e o mesmo stub serve para o Mac. O servidor precisa estar com Warden desligado,
  como já acontece com o build x64 do Windows.
- **OpenGL deprecated no macOS:** funciona hoje, mas a Apple pode removê-lo. O plano de longo prazo é Metal.
- **Divergência do binário original:** o projeto valida funções contra o `Wow.exe` com tags `// OFFSET:`. O código
  Mac deve ficar em pastas próprias (`Mac/`) e não alterar o código validado.

## Índice

| Documento | Conteúdo |
|---|---|
| [01-metricas-e-experimento-clang.md](01-metricas-e-experimento-clang.md) | Números do código e o teste de compilação com clang no macOS, com script para reproduzir |
| [02-build-e-compilador.md](02-build-e-compilador.md) | CMake, flags MSVC, extensões de compilador, header de compatibilidade |
| [03-camada-de-plataforma.md](03-camada-de-plataforma.md) | Os / Storm / BlizzardCore: o que existe, o que reimplementar, mapeamento Win32 → POSIX/Cocoa |
| [04-graficos.md](04-graficos.md) | Gx: backends D3D9/OpenGL, shaders, opções GL legado vs Metal vs tradução |
| [05-audio-e-midia.md](05-audio-e-midia.md) | FMOD Ex, CoreAudio, voice chat, filmes, periféricos |
| [06-rede-login-anticheat.md](06-rede-login-anticheat.md) | Winsock/IOCP, Grunt, Battle.net, Warden, checksum do executável |
| [07-apple-silicon.md](07-apple-silicon.md) | arm64 nativo vs x86_64 + Rosetta 2, SSE, ponto flutuante |
| [08-roadmap.md](08-roadmap.md) | **Plano de etapas E0–E9**: tarefas, critério de pronto e estimativa de cada etapa |
| [09-riscos.md](09-riscos.md) | Riscos e mitigação |
| [10-estrategia-de-repositorio.md](10-estrategia-de-repositorio.md) | Código multiplataforma único, fork, PRs para o upstream e CI |

## Atalho para quem só quer jogar no Mac agora

Antes de qualquer porte, o `Wow64.exe` desta árvore pode ser testado no CrossOver, Wine ou Whisky com
`-opengl` ou D3D9 via DXVK/MoltenVK. Isso não substitui o porte, mas serve de referência de comportamento e
desempenho.
