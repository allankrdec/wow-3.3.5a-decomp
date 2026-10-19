# 09: Riscos

| # | Risco | Probabilidade | Impacto | Mitigação |
|---|---|---|---|---|
| 1 | **A Apple remove o OpenGL** do macOS | baixa a médio prazo, alta a longo | alto: o caminho (a) do Gx para de funcionar | o backend Metal (fase 5) atrás da mesma interface `CGxDevice` |
| 2 | Bugs do driver GL-sobre-Metal da Apple com ARB programs ou fixed-function (artefatos em água, sombras, partículas) | média | médio | validar o modo `-opengl` no Windows primeiro para separar bug do jogo de bug do driver; contornos pontuais; o Metal resolve de vez |
| 3 | Bugs de decompilação que só aparecem no caminho OpenGL, pouco usado no Windows (o padrão é D3D9) | média | médio | fase 0: jogar no Windows com `-opengl` e corrigir lá, o que também beneficia o upstream |
| 4 | Conflito com o projeto upstream: o porte mexe em headers comuns e no CMake | média | médio | PRs pequenos e neutros para o MSVC; código Mac isolado em `Mac/`; conversar no Discord do projeto antes de abrir PRs grandes |
| 5 | Diferença de ponto flutuante no arm64 (FMA, `rsqrt` NEON) causa divergência de física ou animação | baixa | baixo | `-ffp-contract=off`; testes visuais; a simulação autoritativa é do servidor |
| 6 | Mouse-look e captura de cursor no macOS (aceleração, permissões de acessibilidade) | média | médio (jogabilidade) | `CGAssociateMouseAndMouseCursorPosition` + deltas brutos de `NSEvent`; sem precisar de permissão de acessibilidade |
| 7 | IME (CJK) complexo de integrar com o `EditBox` do jogo | média | baixo para público ocidental | adiar para a fase 4 |
| 8 | Servidor exige um hash de executável específico ou Warden | depende do servidor | alto para aquele servidor | é a mesma limitação do build Windows x64 atual; documentar a configuração do servidor (Warden off, sem validação estrita de versionproof) |
| 9 | Rosetta 2 retirado (se o alvo for x86_64) | alta, 2027+ | alto | mirar arm64 nativo desde o início (doc 07) |
| 10 | Page size de 16 KB no Apple Silicon quebra suposições de alocadores (`EmergencyMem`, SMem) | baixa | médio | usar `vm_page_size`; testar cedo |
| 11 | Volume com distinção de maiúsculas (APFS case-sensitive) quebra arquivos soltos (AddOns, WTF) | baixa | baixo | lookup ignorando caixa em `File::ToNative` |
| 12 | Gatekeeper e assinatura: o app não abre em outros Macs | alta se distribuído | baixo | assinatura ad-hoc para uso local; Developer ID + notarização para distribuição |

## Risco legal

O repositório é uma decompilação de software proprietário (`Wow.exe`), e o porte não muda a natureza do código.
O projeto já não distribui dados do jogo, e o porte deve seguir a mesma linha: não incluir MPQs nem binários da
Blizzard, como `Battle.net.dll`/`.dylib` ou `DivxDecoder.dll`. Isso é uma observação, não uma avaliação jurídica.
