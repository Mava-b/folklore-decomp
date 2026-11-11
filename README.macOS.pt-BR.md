# Folklore — recompilação estática para PC (macOS)

🌐 [English](README.macOS.md) · **Português (BR)**

Port nativo do **Folklore** (PS3, disco `BCUS98147` / `BCES00050`) para
**macOS/arm64**, feito com recompilação estática: o código PowerPC e SPU do
jogo é traduzido para C/C++ e compilado como um executável nativo, com
gráficos em Metal. Não é um emulador. Esta página trata da versão para Mac; a
versão para Windows, onde o port começou, está no [README.md](README.md).

**Um jogo novo começa e é jogável no Mac**: a abertura, as cutscenes, Doolin,
as páginas da história e as capturas de Folks. Testado num M4 Pro com macOS
27, montado a partir de uma imagem do disco `BCUS98147` v1.00. Ainda não há
download: você monta o jogo a partir do seu próprio disco com um script
([como](#montar-a-partir-do-código)), ou com o app em
[`launcher/`](launcher/README.md).

O port em si é o port de Folklore para Windows do
[Mava-b](https://github.com/Mava-b/folklore-decomp), feito sobre o
[ps3recomp](https://github.com/sp00nznet/ps3recomp) do sp00nznet. A versão
para Mac acrescenta o toolkit de macOS dos ports do autor de
[inFAMOUS 2](https://github.com/erikomis/infamous2-recomp) e
[Resistance](https://github.com/erikomis/resistance-recomp), e correções
feitas antes nos ports de Demon's Souls, Resistance 2 e PES 2013 (veja
[Correções feitas aqui](#correções-feitas-aqui)).

> [!WARNING]
> **Inicial, ainda não jogado até o fim no Mac.** Um jogo novo vai até a
> história e as capturas funcionam, só no macOS com Apple silicon. Os
> controles de movimento ainda não são lidos (as capturas se completam
> sozinhas), e a maior parte dos extras de PC da versão Windows não está no
> Mac. O que falta está no [ROADMAP.macOS.pt-BR.md](ROADMAP.macOS.pt-BR.md).

> **Este repositório não contém o jogo, o firmware, chaves da Sony nem nada
> traduzido a partir deles.** Cada pessoa gera tudo localmente a partir do
> próprio disco legítimo; o EBOOT é descriptografado pelo decriptador do
> próprio port, sem RPCS3 e sem arquivos de chave. Projeto de fãs, sem
> afiliação com a Sony Interactive Entertainment nem com a Game Republic.

## Estado

| | |
| --- | --- |
| Montar no macOS (Apple silicon) a partir do seu disco com `setup_mac.sh` | ✅ cerca de 2,5 minutos só a compilação, num M4 Pro |
| Boot, instalação do primeiro boot, logos de abertura, vídeos (FFmpeg) | ✅ |
| Tela de título, Options, Load Game, volta ao título | ✅ desde a correção do caminho vazio (a tela preta do "HDD access error") |
| Jogo novo → cutscenes → Doolin → as páginas da história | ✅ |
| Capturas de Folks, inclusive as de chefes | ✅ completadas sozinhas (captura automática): os sensores de movimento ainda não são lidos no Mac |
| As perguntas sim/não e OK do PS3 | ✅ respondidas com Cross/A ou Circle/B, Return/Escape ou o mouse |
| Gráficos: grama e cercas, penhascos e pedras, bloom, cor de blend | ✅ corrigidos no Metal (veja [Correções feitas aqui](#correções-feitas-aqui)) |
| A janela host: MetalFX, janela / tela cheia do macOS / sem bordas, escolha do monitor, vsync | ✅ desde 3b5d899; testado com resolução interna 1,5x e MetalFX para 2560x1440 |
| Resolução interna (`PS3_RES_SCALE`, de 1x a 4x de 1280x720) | 🧪 1,5x testado; as outras não medidas |
| Menu dentro do jogo (F1 / fn+F1 / Cmd+, / L3+R3): imagem, áudio, controles, HUD de desempenho, Sair do jogo | ✅ salvo em `overlay.ini` |
| Controle pelo SDL (controles PlayStation; controles Xbox lidos como HID) | ✅ botões e analógicos (jogado com um controle Xbox Series X) |
| Teclado e mouse (o layout do toolkit, remapeável) | 🧪 mapeados, não testados jogando Folklore |
| Vibração | ⏳ Folklore só vibra na versão 1.10, que os scripts do Mac ainda não montam |
| Controles de movimento (capturar mexendo o controle) | ⏳ ainda não lidos no Mac |
| Pacote de app com Game Mode (`folklore.app`) | ✅ |
| Desempenho | ✅ 60 fps na abertura e em Doolin num M4 Pro (resolução interna 1,5x, MetalFX, SMAA); o fps aparece no título da janela e no HUD |
| O menu do engine e os extras de PC da versão Windows (ultrawide, sombras suaves, ...) | ⏳ só no Windows (Direct3D 12) |
| O app (`launcher/`): instalar, recompilar, jogar, ajustes, saves | 🧪 montado a partir do código, ainda sem release |
| Jogo inteiro | ❓ não jogado até o fim no Mac |

Detalhes e próximos passos no [ROADMAP.macOS.pt-BR.md](ROADMAP.macOS.pt-BR.md).

## Baixar e jogar

Ainda não há release para baixar. Você precisa de um Mac com Apple silicon,
do seu próprio disco de **Folklore**, versão 1.00, norte-americano
`BCUS98147` ou europeu `BCES00050` (a versão japonesa não funciona), como
imagem de disco (`.iso`) ou pasta extraída que contenha `PS3_GAME`, e do
[Homebrew](https://brew.sh).

Dois caminhos do disco até o Play:

1. **O app:** monte o [`launcher/`](launcher/README.md) (Node, Rust e as
   Xcode Command Line Tools), abra e aperte **Instalar o jogo**, escolha seu
   `.iso` ou pasta, depois **Jogar**. Ele roda os próprios scripts deste
   repositório, abaixo, e mostra o progresso deles.
2. **O terminal:** `./setup_mac.sh <seu .iso ou pasta>`, depois
   `./run_mac.sh` ([Montar a partir do código](#montar-a-partir-do-código)).

O seu disco só é lido, nunca escrito: uma imagem de disco é montada como
somente leitura em `gamedisc/`.

## O app

[`launcher/`](launcher/README.md) é um app de desktop (Tauri), adaptado do
launcher do port de Resistance, que veio do de inFAMOUS 2. Ele não refaz o
trabalho do port: roda `setup_mac.sh` e `run_mac.sh`, guarda os ajustes e
inicia o jogo com eles. Páginas: Início, Gráficos, Desempenho, Jogo,
Controles, Áudio e Avançado.

- **Instalar / recompilar** com barra de progresso, um motivo em linguagem
  simples quando uma etapa falha, e **Cancelar**; apertar de novo continua de
  onde parou.
- **Gráficos:** resolução interna, FXAA ou SMAA, nitidez, filtragem
  anisotrópica, gama, vibrância, contraste, vinheta, granulação, uma LUT de
  cor, pacotes de texturas, e perfis prontos (Desempenho / Equilibrado /
  Qualidade).
- **Desempenho:** o limite de fps do próprio jogo (do
  `folklore_settings.ini`), o contador de fps, o HUD, os caches de vértices e
  de shaders.
- **Jogo:** o seu disco, os logos de abertura, o idioma (versão europeia), os
  vídeos, os ajustes salvos do menu dentro do jogo, e um **gerenciador de
  saves** (backup, restaurar, importar e exportar como `.zip`).
- **Controles:** remapear teclado, mouse e controle, com teste de botões;
  opções de mouse e vibração. **Áudio:** volume, mudo, dispositivo de saída.
- **Avançado:** o que este Mac precisa, a pasta do Folklore, opções de
  recompilação, atualizações pelo git, o log do jogo, um `.zip` dos logs para
  um relato, variáveis extras.
- **Atalho Jogar** na Mesa e **Adicionar à Steam**.

As opções da janela host (MetalFX, modo de tela, tamanho da janela,
monitor, vsync) ficam na página Gráficos e nos ajustes rápidos da Home, e o
Avançado tem a opção "usar a janela do próprio jogo"
(`FOLKLORE_HOST_WINDOW=0`). O app não tem feed de releases, então não se
atualiza sozinho; ele é recompilado a partir desta pasta. As builds dele não
são assinadas: na primeira vez, clique com o botão direito > Abrir.

**Dentro do jogo,** F1 (fn+F1), Insert, Cmd+, ou L3+R3 abre o **menu dentro
do jogo** (do port de inFAMOUS 2): imagem, volume do áudio, controles com
remapeamento ao vivo, um HUD de desempenho e **Sair do jogo**. Detalhes
[abaixo](#menu-dentro-do-jogo).

## Problemas conhecidos

- **Controles de movimento:** os sensores de movimento dos controles (o
  giroscópio do SDL) ainda não são lidos no Mac, então toda captura se
  completa sozinha (captura automática), inclusive as de chefes. A versão
  Windows lê os sensores diretamente.
- **Os extras do Windows não estão no Mac:** o menu do engine (Dear ImGui em
  Direct3D 12) e a maior parte da lista "What's better than on the PS3" do
  README.md (ultrawide, oclusão de ambiente, sombras suaves, profundidade de
  campo e bloom suaves, as almas dos Id mais limpas, ...) vivem no renderizador
  Direct3D 12 (`rsx_live_draw.c`), que é só do Windows. No Mac as opções de
  imagem são as do toolkit (variáveis `PS3_*`, o menu dentro do jogo).
- **Conteúdo para download:** importar DLC (licenças `.rap`, **Add DLC from
  RPCS3...**) está só no menu do engine do Windows.
- **A atualização 1.10** ainda não está ligada aos scripts do Mac: a versão
  para Mac é a 1.00 do disco, então sem vibração e sem a tabela de Folks mais
  nova.
- **Engasgos no primeiro uso:** na primeira vez que um efeito aparece, o
  shader dele é traduzido, o que pode travar um instante. O cache de shaders
  (ligado no `run_mac.sh`) guarda esses shaders, então cada engasgo acontece
  uma vez só.
- **O código vetorial do PPU (VMX)** roda pelo caminho portátil, ainda sem
  versão NEON.
- **Windows:** o runtime mesclado (ps3recomp v0.19.0+3 com o toolkit de
  macOS) ainda não foi compilado nem rodado no Windows.

## Em andamento

O que está sendo feito agora (detalhes no
[ROADMAP.macOS.pt-BR.md](ROADMAP.macOS.pt-BR.md)):

- **Jogar mais adiante na história** no Mac, anotando o que quebra.
- **O app:** as opções da janela host e a opção "usar a janela do próprio
  jogo", acima.
- **Controles de movimento** pelo giroscópio do SDL, para as capturas serem
  jogadas como no PS3.
- **A atualização 1.10** no `setup_mac.sh` e no `run_mac.sh`.
- **Uma build Windows do runtime mesclado**, para conferir que nada quebrou
  lá.

## Montar a partir do código

O app faz tudo isso por você; à mão, pelo terminal:

### O que você precisa

- Um Mac com Apple Silicon (testado num M4 Pro, macOS 27).
- O seu próprio disco de Folklore, versão 1.00, `BCUS98147` ou `BCES00050`,
  como `.iso` ou pasta extraída que contenha `PS3_GAME`.
- As Xcode Command Line Tools (`xcode-select --install`) e o
  [Homebrew](https://brew.sh). O `setup_mac.sh` instala os pacotes do
  Homebrew de que precisa (`cmake ninja sdl2 glslang spirv-cross ffmpeg`) e
  cria o próprio ambiente Python.

### Como montar

```sh
git clone https://github.com/erikomis/folklore-decomp.git
cd folklore-decomp
git checkout macos-port
./setup_mac.sh ~/caminho/para/Folklore.iso   # ou a pasta que contém PS3_GAME
./run_mac.sh
```

O `setup_mac.sh` salva o caminho do seu disco em `folklore_mac.env` e depois,
pulando cada etapa já feita:

1. instala os pacotes do Homebrew;
2. cria um ambiente Python para o lifter (`tools/venv-mac`);
3. descriptografa `PS3_GAME/USRDIR/EBOOT.BIN` com o decriptador do próprio
   port em `decrypted/<hash>/EBOOT.elf` e confere que é um EBOOT 1.00
   suportado;
4. traduz o código do jogo (`folklore/tools/relift.sh`, alguns minutos);
5. promove os registradores do guest e compila
   `folklore/build-mac/folklore.app` (cerca de 2,5 minutos num M4 Pro), com
   o `ICON0.PNG` do seu disco como ícone.

Rode de novo sem argumento para recompilar com o mesmo disco. `JOBS=n` define
as compilações em paralelo (padrão 4), `NO_PROMOTE=1` compila o código
traduzido como está, `RELIFT=1` traduz de novo. Cada etapa escreve um log em
`logs/` (`relift-mac.log`, `promote-mac.log`, `configure-mac.log`,
`build-mac.log`).

## Jogar pelo terminal

```sh
./run_mac.sh
./run_mac.sh PS3_AA=smaa PS3_ANISO=16       # argumentos extras são variáveis de ambiente
```

O `run_mac.sh` monta o disco se preciso e inicia o jogo pela janela host
(`folklore.app/Contents/MacOS/folklore-window`), com as opções que o
`run.ps1` liga no Windows (entre elas `GCM_WAITFLIP_DEPTH=1`: sem ela cada
quadro espera o vblank inteiro e o início dos diálogos trava). Os dados
instalados e o cache do jogo vão para `hdd0/` e `hdd1/` aqui, os saves para
`gamedata/dev_hdd0/home/00000001/savedata`, o log para `logs/run-mac.log`, e
as opções do próprio jogo são lidas do `folklore_settings.ini`, como no
Windows.

Um controle funciona direto, pelo SDL. Teclado e mouse funcionam junto com
ele, no layout padrão do toolkit (as teclas Z/X/A/S do próprio Folklore
continuam só no Windows):

| Tecla | PS3 |
| --- | --- |
| W A S D | analógico esquerdo |
| I J K L ou mouse | analógico direito (câmera) |
| Setas | direcional |
| Espaço | ✕ |
| C, Control, Backspace | ○ |
| R, E | □ |
| Q | △ |
| Z / X | L1 / R1 |
| G / 1 | L2 / R2 |
| Shift / V | L3 / R3 |
| Return, Enter do teclado numérico, Esc, P / Tab, M | START / SELECT |
| Botão esquerdo / direito / do meio do mouse | R1 / L1 / R2 |
| Botões 4 / 5 do mouse, roda | L2 / □, △ |

Qualquer atribuição pode ser trocada: na aba **Controles** do menu dentro do
jogo (ao vivo), na página Controles do app, ou num arquivo de controles
(`PS3_INPUT_MAP`, no formato `input.toml` do toolkit, o mesmo do port de
inFAMOUS 2). O mouse tem as opções do toolkit (`PS3_MOUSE=0`,
`PS3_MOUSE_SENS`, `PS3_MOUSE_INVERT_Y`, `PS3_MOUSE_CAPTURE=0`); um clique na
janela captura o mouse, e Esc, Cmd ou sair da janela o soltam.

As perguntas sim/não e OK do PS3 (ao salvar e carregar, entre outras) são
desenhadas pelo menu dentro do jogo: Cross/A ou Return para sim/OK,
Circle/B ou Escape para não, ou o mouse nos botões. O botão que abriu a
pergunta precisa ser solto antes. Com `FOLKLORE_HOST_WINDOW=0` elas aparecem
como uma folha (sheet) na janela do próprio jogo, respondidas do mesmo jeito.

| Atalho | O que faz |
| --- | --- |
| Cmd+F | liga/desliga a tela cheia (sem bordas com `PS3_WINDOW_MODE=borderless`, a do macOS nos outros casos) |
| Cmd+Ctrl+F | liga/desliga a tela cheia do macOS |
| Cmd+Option+F | liga/desliga a tela cheia sem bordas |
| Esc / Cmd | solta o mouse |
| F1 (fn+F1 num teclado de Mac), Insert ou Cmd+, ; L3+R3 no controle | o menu dentro do jogo (abaixo) |
| Cmd+Q, fechar a janela | sai do jogo |

### Menu dentro do jogo

F1 (fn+F1 num teclado de Mac, onde F1 é a tecla de brilho), Insert (Help em
teclados de Mac mais antigos) ou Cmd+, abre e fecha o menu; no controle,
aperte L3 e R3 juntos. Esc ou ○ também fecham. Enquanto ele está aberto o
jogo não recebe comandos, mas continua rodando: pause antes com START se
precisar. `PS3_OVERLAY_PAD=0` desliga a combinação do controle,
`PS3_OVERLAY=0` o menu inteiro. É o menu do port de inFAMOUS 2, adaptado a
este toolkit.

| Aba | O que muda |
| --- | --- |
| Imagem | gama, nitidez, vibrância, contraste, vinheta, granulação, a intensidade da LUT, FXAA ou SMAA, MetalFX, vsync e o modo de tela (janela, tela cheia do macOS, sem bordas), ao vivo; filtragem anisotrópica e resolução interna aparecem e valem a partir do próximo início |
| Áudio | volume geral (vídeos incluídos) |
| Controles | jogar com controle + teclado e mouse, só o controle, ou só teclado e mouse; vibração; a câmera pelo mouse; uma tabela de cada botão do PS3 com a tecla, o botão do mouse e o botão do controle: clique numa célula, aperte o novo comando, vale na hora |
| Desempenho | um HUD pequeno num canto que fica quando o menu fecha: fps, tempo de quadro (com gráfico), núcleos de CPU em uso e threads ocupadas, o tempo de codificação Metal do RSX e a espera pela GPU |

**Sair do jogo** pergunta antes, depois encerra o jogo como fechar a janela
faz, depois que qualquer save em andamento termina de ser gravado.

O que o menu muda é salvo quando ele fecha, em `overlay.ini` ao lado do
`run_mac.sh` (`PS3_OVERLAY_INI`), e o remapeamento em `input.toml` ao lado
dele. Prioridade no início: uma variável definida quando o jogo começa ganha
(o shell, ou o app, que só passa os ajustes fora do padrão); senão a linha do
menu em `overlay.ini`; senão o padrão embutido. O menu marca com "env" um
ajuste cuja variável foi definida.

**Game Mode.** A compilação gera um pacote de app,
`folklore/build-mac/folklore.app` (`folklore/build-mac/folklore` é um link
para o executável dele, e o `run_mac.sh` o inicia direto, então o ambiente e
o log funcionam como antes). O Info.plist dele (do port de inFAMOUS 2) o
marca como jogo, então o macOS 14+ liga o Game Mode enquanto ele está em tela
cheia e na frente. O jogo também impede o macOS de colocá-lo em App Nap
enquanto roda.

| Variável | O que faz |
| --- | --- |
| `PS3_RES_SCALE=1.5\|2\|3` | Resolução interna: os render targets do jogo a 1,5x (1080p), 2x (1440p) ou 3x (4K) de 1280x720. Qualquer valor de 1 a 4 em passos de 0,25, ou `720p`, `1080p`, `1440p`, `4k`. Sem definir ou 1: o tamanho do próprio PS3. O que o código do PS3 lê de volta continua em 720p. Lida no início |
| `PS3_UPSCALE=metalfx` | Upscaling espacial MetalFX até o tamanho em pixels da janela, em vez de um esticamento simples (só na janela host). Padrão desligado |
| `PS3_WINDOW_MODE=windowed\|fullscreen\|borderless` | como o jogo abre: numa janela (padrão), em tela cheia do macOS (um Space só dele) ou em tela cheia sem bordas (cobre a tela no lugar) (só na janela host) |
| `PS3_WINDOW_SIZE=<l>x<a>` | o tamanho da janela em pontos, ex. `1920x1080` (padrão 1280x720, reduzido para caber na tela) (só na janela host) |
| `PS3_DISPLAY=<n>\|<nome>` | a tela onde o jogo abre: `1` é a da barra de menus, `2` a seguinte, ou o nome de uma tela (sem diferenciar maiúsculas, ou parte dele). Padrão: a tela principal (só na janela host) |
| `PS3_VSYNC=0` | apresenta os quadros sem esperar a atualização da tela (menos latência, pode rasgar). Padrão ligado |
| `PS3_AA=fxaa\|smaa` | anti-aliasing na imagem do jogo: FXAA (o mais leve, um pouco suave) ou SMAA 1x (mais nítido). Padrão desligado |
| `PS3_SHARPEN=<0..1>` | nitidez adaptativa ao contraste; 0 (padrão) é desligado |
| `PS3_ANISO=2\|4\|8\|16` | filtragem anisotrópica nas texturas com mipmaps do jogo; tabelas de consulta, render targets e a interface mantêm a filtragem. Padrão desligado |
| `PS3_VIBRANCE=-1..1` | vibrância: a saturação sobe mais nas cores apagadas (negativo atenua). Padrão 0 (desligado) |
| `PS3_CONTRAST=0.5..1.5` | contraste em torno do cinza médio. Padrão 1 (desligado) |
| `PS3_LUT=<arquivo.png>` | uma LUT de cor no formato `LUT.png` do ReShade (N·N de largura, N de altura, ex. 1024x32), arquivo seu. Padrão nenhuma |
| `PS3_LUT_STRENGTH=0..1` | quanto da LUT é misturado. Padrão 1 |
| `PS3_VIGNETTE=0..1`, `PS3_GRAIN=0..1` | cantos mais escuros; granulação de filme. Padrão 0 (desligado) |
| `PS3_MODS=<pacote>,<pacote>` | pacotes de texturas: pastas em `PS3_MODS_DIR` (ou caminhos completos) com PNGs nomeados pela chave da textura que substituem; o primeiro pacote ganha. `PS3_MODS_DUMP=<pasta>` grava ali as texturas de cor do jogo com esses nomes. Padrão nenhum |
| `PS3_INPUT_MAP=<arquivo>` | o arquivo de controles. Sem definir: o `input.toml` que o menu dentro do jogo salvou ao lado do `overlay.ini`, se houver, senão o layout embutido acima. Vazio: o layout embutido |
| `PS3_AUDIO_VOLUME=<0..100>` | volume geral (padrão 100), numa curva que soa uniforme (50 é cerca de −12 dB); os vídeos também |
| `PS3_AUDIO_MUTE=1` | sem som (qualquer valor silencia; deixe sem definir para ouvir o jogo) |
| `PS3_AUDIO_DEVICE=<nome>` | a saída de áudio, pelo nome ou parte dele (`AirPods`); não encontrada ou sem definir, a padrão do sistema |
| `PS3_OVERLAY=0` | sem menu dentro do jogo e sem HUD |
| `PS3_OVERLAY_HUD=1` | o HUD de desempenho (fps, tempo de quadro, CPU/GPU). Ligado por padrão: o `run_mac.sh` o grava no `overlay.ini` na primeira execução, então o menu pode desligar; `0` esconde |
| `PS3_OVERLAY_INI=<arquivo>` | onde o menu dentro do jogo salva; o `run_mac.sh` define `overlay.ini` ao lado dele. Vazio: as mudanças valem só na sessão |
| `PS3_SHADER_CACHE=<pasta>` | guarda os shaders traduzidos entre execuções, e os pipelines compilados junto (`pipelines.metalar`): uma cena só engasga na primeira vez que seus shaders aparecem (traduzir um pode levar 160 ms). Ele sobrevive às recompilações do jogo e só recomeça quando o tradutor de shaders muda; o `run_mac.sh` define `folklore/build-mac/shader_cache`. Vazio ou `0`: desligado |
| `PS3_PIPELINE_CACHE=0` | sem o arquivo de pipelines compilados (as traduções continuam guardadas) |
| `PS3_PIPELINE_TIME=1` | registra no log quanto cada shader novo leva para traduzir e compilar |
| `PS3RECOMP_ENG_VCACHE=0` | desliga o cache de vértices entre quadros (ligado por padrão) |
| `PS3RECOMP_METAL_FPS=1` | o fps no título da janela e linhas `[RSX FPS]` no log, uma vez por segundo. Ligado por padrão no `run_mac.sh` (como nos ports do Resistance); `0` desliga |
| `PS3_VERBOSE=1` | o log completo do runtime em `logs/run-mac.log`; o `run_mac.sh` define 0 (silencioso) |
| `MSGDIALOG_NATIVE=0` | as perguntas do PS3 respondidas automaticamente em vez de mostradas a você |
| `FOLKLORE_HOST_WINDOW=0` | o jogo abre a própria janela em vez da janela host: sem MetalFX, escolha de monitor ou modos de tela pelo menu; as perguntas do PS3 viram uma folha nessa janela. Padrão 1 |
| `FOLKLORE_EXE=<caminho>` | roda outra build do `folklore` (defina no shell: `FOLKLORE_EXE=... ./run_mac.sh`). Padrão `folklore/build-mac/folklore` |
| `PAD_FILE=<arquivo>` | para testes: a cada leitura, uma máscara de botões escrita no arquivo (`0x0040 4`) é apertada por esse número de leituras e o arquivo é esvaziado, para um script conduzir os menus |

## Correções feitas aqui

No ps3recomp (tudo em
[`patches/0001-folklore-on-upstream-de4f0903.patch`](patches/README.md), com
a série antiga de um patch por correção guardada em
`patches/v0.15-39a36296/`):

- **O toolkit, atualizado:** o ps3recomp incluído passou da v0.15
  (39a36296) para a upstream v0.19.0+3 (de4f0903), 789 commits mais nova, com
  os 83 patches do Folklore mesclados nela. A upstream traz a maior parte do
  trabalho dela em macOS / Metal.
- **O toolkit de macOS dos ports de Resistance e inFAMOUS 2**, mesclado: o
  menu dentro do jogo, a resolução interna, MetalFX e modos de tela pela
  janela host, LUTs de cor, remapeamento de comandos, volume do áudio, os
  caches de vértices e de shaders, pacotes de texturas e a trava de saída.
  O escalonador de SPU, a saída do lifter e as mudanças de FPCR deles ficaram
  de fora (o código traduzido e o SPURS do Folklore dependem dos próprios).
- **O guest fora da thread principal:** o guest roda numa thread própria de
  256 MB e a thread principal gira o run loop (do port de Demon's Souls): com
  o guest na thread principal o backend Metal nunca esvaziava o FIFO.
- **Um SIGTRAP a cada três inícios:** o NSApp é preparado na thread principal
  antes de o guest começar, então o AppKit nunca prende a fila de eventos a
  outra thread.
- **A tela preta no título:** o jogo faz stat em `""` toda vez que volta ao
  título; fora do Windows isso apontava para uma pasta real, e o jogo rodava
  o instalador de downloads sobre lixo até "An HDD access error occurred.".
  Um caminho vazio agora é ENOENT, como no Windows.
- **`cellFsUnlink`** nunca apaga uma pasta (EISDIR, como o lv2).
- **As perguntas do PS3** vão para o jogador (uma folha, ou o menu dentro do
  jogo na janela host), e o controle sempre volta ao jogo depois de uma.
- **Buracos nas pedras:** o engine de desenho Metal guardava só 256 pacotes
  de desenho por BEGIN/END, então penhascos e pedras perdiam cerca de um
  quarto dos triângulos. Pacotes contíguos são juntados e até 4096 faixas
  ficam na fila, como o patch 0019 fez para o Direct3D 12.
- **Grama e cercas como cartões sólidos:** alpha-to-coverage emulado como
  teste de alfa, como o patch 0020 fez para o Direct3D 12.
- **Bloom como uma grade de quadrados:** o filtro de convolução do RSX
  aplicado no shader (patches 0035/0036), e **a cor de blend** passada ao
  Metal para os fatores de blend constantes (patch 0038).
- **Dos ports de Resistance 2, PES 2013 e inFAMOUS 2, para sessões longas:**
  tabelas maiores no Metal e no engine de desenho (no Resistance 2 uma tabela
  de pipelines cheia descartava todo desenho novo), um autorelease pool em
  cada passada do relógio de quadros (vazava cerca de 2 GB em três horas),
  uploads inline NV308A em toda a faixa de cor, uma textura linear com mipmaps
  mantendo o pitch em cada nível, e App Nap e agrupamento de timers
  desligados enquanto o jogo roda.

No port: `GCM_WAITFLIP_DEPTH` e as outras opções que o `run.ps1` liga, agora
no `run_mac.sh`; o arquivo de ajustes, a descriptografia de DLC e o
assistente de captura compilados para macOS (`_NSGetExecutablePath`,
CoreGraphics, CommonCrypto, `mach_vm_region`); o assistente de captura numa
thread própria, para as capturas se completarem sem o menu do engine; o
pacote de app e o ícone dele; `setup_mac.sh`, `run_mac.sh` e o app.

## Estrutura

| Caminho | O que é |
| --- | --- |
| `setup_mac.sh`, `run_mac.sh`, `mac_common.sh` | Montar a partir do seu disco; jogar; o que os dois compartilham |
| `folklore_mac.env` | Criado pelo `setup_mac.sh`: o caminho do seu disco |
| [`folklore/`](folklore/) | O port: projeto CMake, o HLE e o código de engine do Folklore; `folklore/macos/` tem o Info.plist do pacote e o script do ícone |
| [`ps3recomp/`](ps3recomp/) | O recompilador e o runtime, com o patch abaixo já aplicado |
| [`patches/`](patches/README.md) | As mudanças deste port no ps3recomp, como um patch sobre a upstream de4f0903 |
| [`launcher/`](launcher/README.md) | O app (Tauri) |
| [README.md](README.md) | A versão para Windows |

## Licença

O código do próprio port é MIT ([LICENSE](LICENSE)); o ps3recomp mantém a
própria licença (MIT). Folklore é © Sony Interactive Entertainment / Game
Republic; este projeto não tem afiliação com a Sony nem com a Game Republic,
e nenhuma parte do jogo está incluída.
