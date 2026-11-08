# Roadmap (macOS)

🌐 [English](ROADMAP.macOS.md) · **Português (BR)**

Estado do port nativo do Folklore em macOS/arm64 (disco versão 1.00,
`BCUS98147` e `BCES00050`; a versão para Mac foi testada com o `BCUS98147`)
e o que vem a seguir. A versão Windows está no [README.md](README.md) e o
diário de engenharia dela em
[`folklore/docs/STATUS.md`](folklore/docs/STATUS.md). `[x]` = feito e
verificado; `[ ]` = a fazer.

## Resumo da situação (2026-11-07)

- **Roda:** a compilação a partir do seu próprio disco com `setup_mac.sh`;
  boot, os logos de abertura e os vídeos, a tela de título, Options e Load
  Game, um jogo novo até as cutscenes, Doolin e as páginas da história, com
  as capturas de Folks (inclusive as de chefes) se completando sozinhas.
  Testado num M4 Pro com macOS 27.
- **A janela:** desde 3b5d899 o jogo roda pela janela host do ps3recomp
  (`folklore-window`, dentro do `folklore.app`): MetalFX, janela / tela cheia
  do macOS / sem bordas, escolha do monitor, vsync. Testado com resolução
  interna 1,5x e MetalFX para 2560x1440. `FOLKLORE_HOST_WINDOW=0` abre a
  janela do próprio jogo como antes.
- **Toolkit:** o ps3recomp atualizado para a upstream v0.19.0+3 (de4f0903)
  com os patches do Folklore mesclados nela, depois o toolkit de macOS dos
  ports de Resistance e inFAMOUS 2 mesclado (menu dentro do jogo, resolução
  interna, remapeamento, caches). Tudo isso é um patch só,
  `patches/0001-folklore-on-upstream-de4f0903.patch` (gerado de novo em
  731fb78).
- **Velocidade:** ainda não medida em números no Mac.
- **O que falta para "jogável no Mac":** jogar mais adiante na história e
  corrigir o que aparecer, uma sessão longa para velocidade e estabilidade, e
  os controles de movimento.
- **Ainda não:** sensores de movimento, a atualização 1.10 nos scripts do
  Mac, o menu do engine e os extras de PC do Windows em Metal, importar DLC no
  Mac, uma build Windows do runtime mesclado, uma release do app.

---

## O que falta para ficar 100% jogável

### Bloqueantes

- [ ] Jogar a história no Mac além dos primeiros capítulos, anotando o que
  quebra. Até agora: um jogo novo até Doolin e as páginas da história.
- [ ] Uma sessão longa em janela (uma hora ou mais) para o fps e qualquer
  travamento ou vazamento. As correções de sessão longa dos outros ports
  estão aqui (bbce67d), ainda não conferidas por horas.
- [ ] Medir o fps no Mac (`PS3RECOMP_METAL_FPS=1`, o HUD do menu dentro do
  jogo), em 1x e com `PS3_RES_SCALE`, e com o limite de fps do próprio jogo
  acima de 30 (o `frame_rate_cap` do `folklore_settings.ini`, lido no Mac;
  não medido).

### Gráficos

- [ ] Comparar com a versão Windows, cena a cena, para achar o que o
  renderizador Metal ainda desenha diferente. Corrigidos até agora: grama e
  cercas, buracos em penhascos e pedras, a grade no bloom, a cor de blend
  (veja Feito).
- [ ] Os extras de PC da versão Windows (ultrawide, oclusão de ambiente,
  sombras suaves, profundidade de campo e bloom suaves, as almas dos Id mais
  limpas, texturas mais finas ao longe) vivem no renderizador Direct3D 12
  (`rsx_live_draw.c`); nenhum deles está no Metal. Cada um precisa ser
  portado separadamente.
- [ ] Engasgos de shader no primeiro uso: o cache de shaders limita a uma vez
  por shader; `PS3_SHADER_ASYNC=1` (compila em segundo plano) ainda não foi
  testado no Folklore.

### Estabilidade

- [x] O SIGTRAP a cada três inícios, mais ou menos (a fila de eventos do
  AppKit presa a outra thread): corrigido em b1b5c35, seis inícios seguidos
  rodaram.
- [x] A tela preta depois de Options ou Load Game ("An HDD access error
  occurred."): corrigida em 65ff3ed (um caminho vazio é ENOENT).
- [x] O controle preso longe do jogo depois de uma pergunta (o título parava
  de aceitar START): corrigido em c585e18, o controle volta em no máximo
  1,5 s.
- [ ] Sessões de horas: veja Bloqueantes.

### Saves

- [x] Saves pelo cellSaveData do toolkit, em
  `gamedata/dev_hdd0/home/00000001/savedata` ao lado do `run_mac.sh`, a mesma
  estrutura da versão Windows; o app lista, faz backup e restaura.
- [ ] Salvar e carregar ao longo de uma sessão longa no Mac: a conferir.
- [ ] Levar saves entre as versões Windows e Mac: mesma estrutura, não
  testado.

### Controles

- [x] Controle pelo SDL: botões e analógicos.
- [x] As perguntas sim/não e OK do PS3 respondidas pelo jogador: uma folha na
  janela do jogo (f8ced14), desenhadas pelo menu dentro do jogo na janela
  host (3b5d899).
- [x] Remapeamento: a aba Controles do menu dentro do jogo (ao vivo), o app,
  ou `PS3_INPUT_MAP`.
- [ ] **Controles de movimento:** ler o giroscópio dos controles pelo SDL,
  para uma captura ser feita mexendo o controle como no PS3. Até lá o
  assistente de captura completa todas as capturas sozinho.
- [ ] Teclado e mouse: o layout do toolkit (Espaço = Cross, WASD) funciona
  pela janela; não testado jogando Folklore. Um layout pensado para o
  Folklore (a versão Windows lê Z/X/A/S) seria melhor que o layout de tiro que
  o toolkit traz.
- [ ] Vibração: vem com a versão 1.10 (próximo item).

### Rebuild

- [x] Um script do disco até o jogo: `setup_mac.sh` (pacotes do Homebrew,
  ambiente Python, descriptografia com o decriptador do próprio port,
  tradução, promoção de registradores, a compilação). A compilação em si leva
  cerca de 2,5 minutos num M4 Pro.
- [ ] **A atualização 1.10** nos scripts do Mac: o `setup_mac.sh` reconhece o
  tamanho do EBOOT 1.10, mas não recebe a pasta da atualização (o `setup.ps1
  -UpdateDir` do Windows), então uma build para Mac é sempre a 1.00 do disco.
  A versão 1.10 traz vibração e a tabela de Folks mais nova que as DLCs
  posteriores usam.
- [ ] **Uma build Windows do runtime mesclado:** o ps3recomp v0.19.0+3 com o
  toolkit de macOS ainda não foi compilado nem rodado no Windows.

## Depois de jogável (melhorias)

### Gráficos e resolução

- [x] Resolução interna (`PS3_RES_SCALE`, de 1x a 4x), upscaling espacial
  MetalFX, modos de tela, escolha do monitor e vsync, pela janela host.
- [x] FXAA / SMAA, nitidez, filtragem anisotrópica, vibrância, contraste,
  vinheta, granulação, LUTs de cor, pacotes de texturas: da mesclagem do
  toolkit.
- [ ] Ultrawide e os outros extras do Windows em Metal (veja Gráficos acima).
- [ ] Upscaling temporal MetalFX, HDR (EDR): não começados.

### App para instalar e jogar (launcher)

- [x] O app em `launcher/` (Tauri, a partir dos launchers de Resistance e
  inFAMOUS 2): instalar e recompilar com progresso, ajustes, editor de
  controles, gerenciador de saves, log do jogo, um `.zip` dos logs, atalho
  Jogar, Adicionar à Steam.
- [ ] (sendo feito agora) As opções da janela host de volta na página
  Gráficos (MetalFX, modo de tela, tamanho da janela, monitor, vsync), e uma
  opção no Avançado "usar a janela do próprio jogo"
  (`FOLKLORE_HOST_WINDOW=0`).
- [ ] Uma release: um `.dmg` assinado e um feed, para o app se atualizar
  sozinho.
- [ ] Importar DLC no Mac: o **Add DLC from RPCS3...** do menu do engine do
  Windows (licenças `.rap`); o `engine_dlc.cpp` já compila no macOS
  (CommonCrypto), o menu em volta dele não.

### Desempenho

- [ ] Um caminho NEON para o código vetorial do PPU (VMX): hoje o `ppu_vmx.h`
  usa o caminho portátil no arm64.
- [ ] Perfilar uma cena pesada (as ruas de Doolin, um chefe) quando o fps
  estiver medido.

### Outras plataformas

- [ ] A versão Windows continua sendo a principal (a do Mava-b); o trabalho
  no Mac deve mantê-la compilando (veja Rebuild).

### Fora do escopo por enquanto

- [ ] O próprio menu do engine do Windows (Dear ImGui em Direct3D 12): no Mac
  os ajustes dele são o menu dentro do jogo do toolkit e as variáveis `PS3_*`,
  mais o `folklore_settings.ini` para as opções do próprio jogo.

## Feito e finalizado

### Compilação e pipeline

- [x] O jogo compila para macOS em Apple silicon (1e356fd): o arquivo de
  ajustes achado com `_NSGetExecutablePath`, o tamanho da tela pelo
  CoreGraphics, a descriptografia de DLC com CommonCrypto, sem `-march` de
  x86 no arm64, o FFmpeg do Homebrew para os vídeos, a mesma pilha de 256 MB
  na thread principal que o Windows tem.
- [x] `setup_mac.sh`, `run_mac.sh`, `mac_common.sh` (78d80a8): uma imagem de
  disco montada como somente leitura em `gamedisc/`, o EBOOT descriptografado
  pelo decriptador do próprio port, as opções que o `run.ps1` liga
  (`GCM_WAITFLIP_DEPTH` entre elas).
- [x] `folklore.app` com Game Mode (853ee36), com o ícone feito do
  `ICON0.PNG` do seu disco na instalação (nunca distribuído); os ajustes do
  menu dentro do jogo em `overlay.ini`, os shaders compilados em
  `folklore/build-mac/shader_cache`, `FOLKLORE_EXE` para rodar outra build.
- [x] A janela host (3b5d899): o `folklore-window` apresenta os quadros do
  jogo com MetalFX e os modos de tela; fechá-la encerra o jogo de forma
  limpa.

### Do título ao gameplay

- [x] Boot, logos de abertura, vídeos, a tela de título.
- [x] Options e Load Game, e a volta ao título (65ff3ed).
- [x] Jogo novo, cutscenes, Doolin, as páginas da história.
- [x] As capturas se completam sozinhas no macOS, inclusive as de chefes: o
  assistente de captura roda numa thread própria lá (ef35b63); o localizador
  de capturas (F5-F9) lê a memória do guest com `mach_vm_region`.

### Gráficos

- [x] Grama e cercas de arame como cartões sólidos: alpha-to-coverage como
  teste de alfa (f8ced14, como o patch 0020 para o Direct3D 12).
- [x] Buracos em penhascos e pedras: sem o limite de 256 pacotes por desenho
  (f628bd5, como o patch 0019).
- [x] Bloom como uma grade de quadrados: o filtro de convolução no shader; a
  cor de blend passada ao Metal (25c4701, como os patches 0035/0036 e 0038).
- [x] Níveis de mip pequenos desenhados como listras (o pitch de uma textura
  linear com mipmaps), uploads inline NV308A em toda a faixa de cor
  (bbce67d).

### Estabilidade

- [x] O guest numa thread própria de 256 MB, a thread principal girando o run
  loop (do port de Demon's Souls; ac8c6cd).
- [x] O NSApp preparado na thread principal antes do guest (b1b5c35).
- [x] `cellFsUnlink` nunca apaga uma pasta (f91c283).
- [x] Tabelas maiores no Metal e no engine de desenho, um autorelease pool por
  passada do relógio de quadros (antes vazavam cerca de 2 GB em três horas),
  App Nap e agrupamento de timers desligados (bbce67d, dos ports de
  Resistance 2, PES 2013 e inFAMOUS 2).

### Toolkit

- [x] O ps3recomp da v0.15 (39a36296) para a upstream v0.19.0+3 (de4f0903),
  789 commits, com os 83 patches do Folklore mesclados (ac8c6cd).
- [x] O toolkit de macOS de Resistance / inFAMOUS 2 mesclado (76b64d4): 341
  trechos em conflito em 64 arquivos; o que o Folklore roda por padrão não
  mudou.
- [x] Um patch só para tudo isso sobre a de4f0903 (0efc356, gerado de novo em
  731fb78); a série antiga de 85 patches guardada em
  `patches/v0.15-39a36296/`.

### Créditos

- [Mava-b](https://github.com/Mava-b/folklore-decomp): o port do Folklore em
  si, o SPURS dele, as correções do renderizador e a versão Windows.
- [sp00nznet](https://github.com/sp00nznet/ps3recomp): o ps3recomp.
- Os ports do autor de Demon's Souls, Resistance 1 e 2, inFAMOUS 2 e PES
  2013: o toolkit de macOS e as correções citadas acima.
