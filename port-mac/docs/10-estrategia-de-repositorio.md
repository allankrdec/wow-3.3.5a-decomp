# 10: Estratégia de repositório (Windows + Mac)

**Decisão (2026-10-19):** manter **um único código multiplataforma** que compila para Windows e macOS. Não haverá
uma branch só de Mac.

## Por quê

- O código já separa a plataforma em pastas `Win32/` e `Win64/`. O Mac entra como `Mac/`, e o CMake escolhe pela
  plataforma.
- Cerca de 90% do código é comum. As mudanças do porte no código comum (etapa E1) são neutras para o MSVC.
- O upstream (`SatyPardus/wowdecomp`) está ativo. Uma branch só de Mac exigiria trazer correções à mão para
  sempre e teria conflitos nos arquivos comuns que o porte precisa tocar (CMake, headers de SO).
- O build Windows (`Wow64.exe -opengl`) é a referência para separar bug do porte de bug da decompilação.

## Organização

1. **Fork próprio** do repositório:
   - `main` acompanha o upstream;
   - `port-mac` é a branch de trabalho, onde Windows e Mac compilam juntos e que recebe merges frequentes da
     `main`.
2. **PRs pequenos para o upstream** com as mudanças neutras: correções para o clang, `Compat.h`, remoção de
   `windows.h` vestigiais. Se o upstream aceitar as pastas `Mac/`, o porte passa a fazer parte do projeto.
3. **CI no GitHub Actions** compilando as duas plataformas a cada push:
   - `windows-latest`: x64 (MSVC) e Win32 (MSVC + NASM);
   - `macos-latest` (arm64): build Mac e `port-mac/tools/check-clang.sh`.

## Regras

- O código Mac fica em pastas `Mac/`. As funções com `// OFFSET:` não mudam de comportamento.
- O build Win32, usado para comparar com o binário original, continua compilando o mesmo código de antes.
- Toda mudança no código comum precisa compilar nas duas plataformas. A CI garante isso.
- `#if` de plataforma só na camada de SO (Os, Storm, BlizzardCore e headers de plataforma), nunca na lógica de
  jogo.
