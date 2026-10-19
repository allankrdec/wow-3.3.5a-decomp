# 07: Apple Silicon (arm64) vs Intel (x86_64)

Há dois alvos possíveis:

| Alvo | Prós | Contras |
|---|---|---|
| **x86_64 macOS** (roda em Intel nativo e em Apple Silicon via Rosetta 2) | SSE, `__cpuid` e o modelo de FPU continuam funcionando; menos variáveis na primeira fase | a Apple vai retirar o Rosetta 2 (anunciado para depois do macOS 27, mantendo só um subconjunto para jogos antigos); desempenho de 70% a 85% do nativo |
| **arm64 macOS** (nativo) | desempenho máximo; sem prazo de validade | exige tratar SSE, cpuid e FPU, listados abaixo |

> **Decisão (2026-10-19):** o alvo é **arm64 nativo** (`CMAKE_OSX_ARCHITECTURES=arm64`). O x86_64 e o
> universal binary ficam fora do escopo. Por isso, os itens abaixo (SSE → sse2neon, `cpuid` → sysctl, page size
> de 16 KB, `-ffp-contract=off`) são obrigatórios desde a fase 1, e não otimizações para depois.

## O que muda no arm64

### Intrínsecos SSE (`_mm_*`)

Há 313 ocorrências em 6 arquivos (sem contar o FMOD, que no Mac não usa SIMD).

| Arquivo | Usos | Natureza |
|---|---:|---|
| `WoW/Common/ComponentCore/CharacterComponent.cpp` | 73 | composição de texturas de personagem |
| `Engine/Source/Model2/M2Model.cpp` | 21+ | skinning e transformação de vértices (`__m128`) |
| `Engine/Source/Model2/M2Scene.cpp` | 14 | culling e transformação |
| `Engine/Source/ComSat/SPIRIT/SpiritPitch.cpp` | 5 | `_mm_rsqrt_ss` |
| `Engine/Source/ComSat/SPIRIT/SpiritFrame.cpp` | 1 | `_mm_cvtss_si32` |

Opções:

1. **sse2neon** (header único, licença MIT): traduz `_mm_*` para NEON. Zero mudança de código, bom desempenho.
   Atenção: `_mm_rsqrt_ss` tem precisão diferente no NEON (`vrsqrte` + Newton); o sse2neon já trata isso.
2. Reescrever com `<arm_neon.h>`, ou em escalar, só nos trechos quentes.

**Recomendação:** começar com sse2neon e medir. Se o perfil mostrar gargalo, otimizar M2 em NEON.

### `__cpuid`

`BlizzardCore/.../Hardware/Win64/Hardware.cpp` e `Os/Win64/System.cpp` usam `__cpuid` (16 usos) para identificar
o processador (vendor, família, SSE e afins). No Mac:

- `sysctlbyname("machdep.cpu.brand_string")` e `hw.optional.*`;
- devolver capacidades equivalentes: no arm64, sinalizar SSE/SSE2 como "presentes", já que os caminhos SSE passam
  pelo sse2neon.
- `OsGetProcessorId()` (`Os/Win64/System.cpp:299`) retorna `0x783836` (`'x86'`), que vai no logon Grunt
  (`m_processor`). Manter `'x86'` também no arm64 para não confundir o servidor.

### Ponto flutuante

- `WoW/Source/Win64/WinMain.cpp:30-31` chama `_clearfp()` e
  `_control87(0x9001F & ~(_MCW_PC | _MCW_IC), _MCW_DN | _MCW_EM | _MCW_RC)`. Decodificando:
  - todas as exceções de FP mascaradas (`_EM_*` = 0x8001F);
  - arredondamento ao mais próximo (`RC = 0`);
  - denormais preservados (`_DN_SAVE`, sem flush-to-zero).

  **É exatamente o padrão do arm64 no macOS** (FPCR com exceções desligadas, RN, FZ = 0). No Mac basta um
  `feclearexcept(FE_ALL_EXCEPT)`, sem mais nada.
- Precisão: o alvo Win32 usa x87 (`/arch:IA32 /fp:fast`) para comparar com o binário original, mas o alvo x64 já
  usa SSE escalar. O arm64 usa NEON/VFP escalar com IEEE-754 de 32 e 64 bits, o mesmo comportamento do x64.
  **Diferença relevante:** o clang pode contrair `a*b+c` em FMA no arm64 (`-ffp-contract=on` é o padrão), e isso
  muda resultados no último bit. Para manter paridade com o x64, compilar com **`-ffp-contract=off`**.
- `lrintf`, `_isnan` e `_finite`: usar os equivalentes padrão (doc 02).

### `char` com sinal

Atenção: no **arm64 Apple, `char` é com sinal**, como no x86. (No Linux arm64 é sem sinal.) Não há mudança aqui.

### Ordem de bytes, alinhamento e tamanhos

- arm64 é little-endian, como o x86.
- `long` tem 64 bits no macOS (LP64) e 32 bits no Windows (LLP64). O código de jogo usa `int32_t`/`uint32_t`, e
  `long` só aparece no FMOD, em libs C e em ~6 pontos sem impacto de layout (`Glue/ScanDLLGlue.cpp`,
  `WowConnectionNet.cpp:9`). **Conferir esses pontos** ao portar.
- `wchar_t` tem 32 bits no Mac e 16 no Windows. Não há uso fora do código Win32, UILCD e Battlenet/Win64.
- Ponteiros de 64 bits: o alvo x64 já existe e roda, então o código já está 64-bit-clean.
- Acessos desalinhados: o arm64 aceita leituras desalinhadas comuns. `#pragma pack(1)` funciona, mas acesso
  atômico desalinhado não. Os `Interlocked*` não operam em campos `pack`.

### Page size

O Apple Silicon usa páginas de **16 KB**; o x86 usa 4 KB. Revisar `VirtualAlloc`/SMem e qualquer código que
assuma 4096, como `EmergencyMem` e os alocadores de blocos. Usar `getpagesize()` ou `vm_page_size`.

### W^X / JIT

Não se aplica. O código não gera código em tempo de execução: o Warden e o BLLLoader estão desativados no x64.
O Lua 5.1 é interpretado.
