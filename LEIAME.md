# Midnight Club 3: DUB Edition Remix no Xbox 360

Meu port nativo de Midnight Club 3: DUB Edition Remix (Xbox original, versão americana) para Xbox 360 com RGH/JTAG. [Read in English](README.md).

Não é emulador nem retrocompatibilidade. O código x86 do jogo é traduzido para C++ num PC e compilado para o processador PowerPC do 360, com o kernel do Xbox, o Direct3D 8 e o DirectSound refeitos sobre as bibliotecas do próprio 360. O resultado é um `default.xex` de verdade, gerado a partir da sua cópia do jogo.

**Nada do jogo aqui.** Este repositório não tem arquivos do jogo, XEX pronto nem SDK. Você precisa da sua própria cópia do jogo e de instalar as ferramentas. O código gerado e o XEX contêm o código do jogo, então não os compartilhe.

## Situação

- Abre os menus, a carreira, o modo livre e as corridas. Música, som dos motores, HUD, minimapa, desfoque de velocidade e a luz dos faróis no chão funcionam.
- A imagem original de 720x480 é desenhada com qualidade total e mostrada em 1280x720, widescreen.
- O tempo da corrida segue o tempo real: quando um quadro demora mais, o jogo avança seus próprios passos para acompanhar, e os carros e o cronômetro não ficam em câmera lenta.
- O desempenho no console está na tabela abaixo.
- Os vídeos de abertura (Rockstar, Rockstar San Diego e a abertura do jogo) tocam, assim como os vídeos tutoriais.
- Ainda não testei no console a carreira inteira, salvar e carregar, nem os replays.

## Desempenho

Medido no meu console RGH, em 720p, com o `trafego.txt` e o `lod.txt` padrão. O fps depende principalmente de quanto da cidade aparece na tela, então muda bastante ao longo da mesma pista.

| Onde | FPS |
|---|---|
| Menus e telas de abertura | cerca de 22 |
| Ruas leves (áreas abertas, pouca coisa em volta) | 19 a 26 |
| Ruas médias | 16 a 18 |
| Ruas pesadas (centro, muitos prédios e trânsito) | 14 a 15 |

O jogo foi feito para 30 fps no Xbox original. Aqui o limite é o processador: o código x86 traduzido roda mais devagar no PowerPC do 360 do que o original rodava no processador dele. Mesmo assim, o tempo da corrida segue o tempo real, então um fps menor não deixa a corrida em câmera lenta.

## Do que você precisa

- Um Xbox 360 com RGH ou JTAG.
- Sua própria cópia do jogo, versão americana, extraída do disco: `default.xbe` e todos os arquivos e pastas ao lado dele. O SHA-1 do XBE tem de ser `5bca1cf092edaadf876e8f80129900407a4a07b8`.
- Windows 64 bits. Eu compilo no Windows 11.
- **Visual Studio 2010**. O SDK precisa dele. Eu uso a edição Ultimate.
- **Xbox 360 SDK 2.0.21256** (XDK), instalado depois do VS 2010. O instalador define a variável `XEDK`. Confira com `echo %XEDK%`.
- **Visual Studio 2022 Build Tools**, com a carga "Desenvolvimento para desktop com C++". Traz MSVC, CMake e Ninja, usados nas ferramentas do PC.
- **Git**. O CMake usa para baixar o Zydis e o toml++.
- **Python 3**. Sem pacotes extras.

## Compilando

1. Copie o seu `default.xbe` para `pc-recomp\assets\default.xbe`.

2. Compile as ferramentas do PC (o analisador de XBE e o recompilador). Rode `tools_build.bat`, ou abra o **x64 Native Tools Command Prompt for VS 2022** na pasta do projeto e rode:

   ```
   cmake -S x360/tools -B x360/tools/build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build x360/tools/build
   ```

3. Compile o runtime (kernel, Direct3D, áudio e as correções do jogo) para o 360:

   ```
   powershell -ExecutionPolicy Bypass -File x360\build_runtime.ps1
   ```

4. Traduza o jogo, compile e gere o XEX:

   ```
   python build_all.py
   ```

   Isso cria `pc-recomp\generated` a partir do seu XBE, compila em `x360\build\gen` e gera `x360\build\default.xex`. Se o compilador do VS 2010 der erro interno em alguma função, ou o linker falhar num arquivo na otimização do link, o script separa e tenta de novo. No meu PC a compilação inteira leva uns 15 minutos, quase tudo no link.

   Para gerar de novo depois de mudar só o runtime, rode `x360\build_runtime.ps1` e depois `x360\build_xex.ps1`.

## Instalando no console

1. Crie uma pasta no HD ou num pendrive e copie para ela todos os arquivos e pastas do disco do jogo, incluindo o `default.xbe`.
2. Coloque o `default.xex` na mesma pasta. O XBE tem de ficar lá, porque o port lê os dados dele.
3. Abra o `default.xex` pelo Aurora, FreeStyle Dash ou XeXMenu.
4. Os saves ficam em `hdd`, dentro da pasta do jogo. O log é salvo como `mc3_360.log` na mesma pasta.

Back + Start por 2 segundos volta para o dashboard.

## Arquivos de opção

Arquivos na pasta do jogo, ao lado do `default.xex`:

| Arquivo | Efeito |
|---|---|
| `trafego.txt` | Dois números de 0.2 a 1.0: densidade de trânsito e de pedestres. `1.0 1.0` é o jogo original, `0.7 0.7` é o padrão, números menores deixam as ruas mais vazias |
| `lod.txt` | Um número de 0.4 a 1.0: a que distância os modelos trocam para a versão detalhada e os objetos da cidade são desenhados. `1.0` é o original, `0.75` é o padrão |
| `aspect43.txt` | Arquivo vazio: avisa o jogo que a TV é 4:3 |
| `sem_widescreen.txt` | Arquivo vazio: 720p em 4:3 |
| `sem_720p.txt` | Arquivo vazio: saída em 640x480 |
| `semintro.txt` | Arquivo vazio: pula os vídeos de abertura |
| `nogamma.txt` | Arquivo vazio: mantém o gamma de saída da TV como está |
| `debug_buttons.txt` | Arquivo vazio: L3 salva uma captura de depuração, Back muda o gamma |

## Créditos

O runtime vem dos meus ports de [Jet Set Radio Future](https://github.com/EduDicaseGameplay/JetSetRadioFuture-X360) e [Bloody Roar Extreme](https://github.com/EduDicaseGameplay/BloodyRoarExtreme-X360) para Xbox 360, que usam a ferramenta [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) e o [BloodyRoarExtremeRecomp](https://github.com/bromurok/BloodyRoarExtremeRecomp). Várias correções seguem o port para Windows [MidnightClub3-Port](https://github.com/christiand0797/MidnightClub3-Port): o relógio de passo fixo, os replays, os eventos da carreira, a cópia de profundidade dos faróis, o nível de detalhe e a densidade de trânsito. A correção da música WMA segue o mc3port. Detalhes e licenças em `THIRD_PARTY.txt` e `pc-recomp`.

Midnight Club 3: DUB Edition Remix pertence à Rockstar Games e à Take-Two Interactive. Este é um projeto de fã, sem ligação com elas.
