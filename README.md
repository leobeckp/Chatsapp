# Chatsapp
Mensageiro P2P criado em C como projeto da disciplina SSC-0641 - Redes de Computadores, ministrada pelo Prof. Dr. Julio Cezar Estrella auxiliado pelo estagiário PAE Carlos Henrique Gomes Ferreira, na Universidade de São Paulo, campus São Carlos.

# Requerimentos
- Biblioteca NCURSES (http://www.gnu.org/software/ncurses/);
- Sistema operaciona Linux.

# Funções disponíveis
- Adicionar Contato;
- Listar Contatos;
- Remover Contato;
- Enviar Mensagem;
- Enviar Mensagem em Grupo;
- Alterar Nome.

# Como compilar
- Primeiro, instale a biblioteca NCURSES. Para instalá-la, siga esses passos:
  - Baixe-a do site oficial, de preferência a versão 5.9 (http://ftp.gnu.org/pub/gnu/ncurses/)
  - Navegue pelo console até a pasta onde você baixou
  - Execute as seguintes operações:
      - tar zxvf ncurses&lt;version&gt;.tar.gz   #onde &lt;version&gt; deve ser substituído pela versão baixada
      - cd ncurses&lt;version&gt;
      - ./configure --prefix=/caminho/para/alguma/pasta/   #caminho para alguma pasta com permissão de escrita
      - make
      - make install
- Após instalar a biblioteca, abra o arquivo Makefile e substitua a linha "NCURSES_PATH=/set/me/" para o caminho onde você instalou a biblioteca, especificado no comando "./configure"
- Depois de alterá-lo, basta estar na pasta do projeto pelo console e digitar "make all"

# Como utilizar
Para utilizá-lo, basta navegar nos menus com as setas para cima e para baixo do teclado e a tecla ENTER para selecionar alguma opção. As demais teclas de controle estão especificadas na opção "Ajuda" do Menu Principal.

# Autores
- Fernando Tristão Pacheco, Nº USP 8641299;
- Leonardo Beck Prates, Nº USP 7962121.
