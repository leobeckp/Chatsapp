#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>   // Definicao de tipos
#include <sys/socket.h>  // Biblioteca de estrutara de sockets
#include <netinet/in.h>  // Define os padroes de protocolo IP
#include <arpa/inet.h>   // Converte enderecos hosts
#include <unistd.h> // Define constantes POSIX
#include <errno.h>  // Utilizado para capturar mensagens de erro
#include <stdarg.h>
#include <stdint.h>
#include <curses.h>
#include <menu.h>
#include <regex.h>
#include <fcntl.h>
#include <locale.h>
#include <time.h>
#include <semaphore.h>
#include <wchar.h>

#define PORT 41000 //Porta para recepção de conexão
#define MAX_CLIENTS 20 //Máximo de contatos possível
#define TRUE 1
#define FALSE 0
#define MAX_LINES 30 //Máximo de linhas no console
#define MAX_CHARS 300 //Máximo de caracteres em uma mensagem.
#define MAX_NAME 40
#define BOOL int
#define WHITEONBLACK 1
#define REDONBLACK 2
#define GREENONBLACK 3
#define BLUEONBLACK 4
#define CYANONBLACK 5
#define LOGFILE_PATH "./"
#define NO_TIMEOUT -1


//Struct referente ao nó da lista encadeada de clientes.
typedef struct clients_node
{
	int connected; //Identificador do socket relacionado ao cliente.
	pthread_t connectedThread; // Thread de recepção de pacotes relacionada ao cliente.
	struct clients_node* next; //Próximo nó da lista encadeada de clientes.
	char name[MAX_NAME * 2 + 1]; //Nome do cliente.
	struct in_addr ip;	//Ip do cliente.
	struct in_addr ipRcv; //Utilizado na sincronia de contatos, IP de quem solicitou a sincronia, para evitar de adicionar a si mesmo.

} sClient;

int mainSock, connected, ov = 1,sin_size,log_level = -1,clientsCount = 0, consoleHead = 0, consolePos = 0, consoleColor[MAX_LINES], openDay = -1;
struct sockaddr_in server_addr, client_addr;
pthread_t listenerThread, processContactsThread; //Threads do socket para recepção de conexões dos contatos e de sincronia de contatos.
sClient* clients = NULL;//Cabeça da lista encadeada de clientes
sClient* contactsToProcess = NULL; //Cabeça da lista encadeada de contatos a processar para a sincronia.
char myName[MAX_NAME * 2 + 1], consoleBuffer[MAX_LINES][MAX_CHARS * 3 + 1];
BOOL running;
WINDOW *base_win, *mainMenu_win, *console_win;
sem_t messageReceived,processContacts;
pthread_mutex_t processingContactsMutex,clientsMutex,consoleWriteMutex,consoleRefreshMutex;
FILE* logfile = NULL;
void (*onClientClose)(sClient*),(*onPacketReceived)(sClient*,char*,char*), (*onClientConnected)(sClient*);//Funções chamadas pelo servidor quando algum evento especificado pelo nome da função acontece.

/* ----------------------------- INÍCIO DO MÓDULO DO SERVIDOR/CLIENTE ----------------------------- */

/*
Função getClientBySocket: Procura a entrada na lista de clientes correspondente ao socket especificado.
Entrada:
	- socket (int) : Identificador do socket que se quer obter os dados.
Saída: 1 pointero de struct sClient (sClient*).
*/
sClient* getClientBySocket(int socket)
{
	pthread_mutex_lock(&clientsMutex);
	sClient* aux = clients;
	
	while(aux != NULL && aux->connected != socket)
		aux = aux->next;
	pthread_mutex_unlock(&clientsMutex);

	return aux;
}
/*
Função addOnClientsList: Adiciona a entrada especificada na lista de clientes.
Entrada:
	- node (sClient*) : Entrada a ser adicionada na lista.
Saída: nenhuma.
*/
void addOnClientsList(sClient* node)
{
	pthread_mutex_lock(&clientsMutex);
	sClient* aux = clients;
	clientsCount++;

	if(clients == NULL)
	{
		clients = node;	
	}
	else
	{
		while(aux->next != NULL)
			aux = aux->next;
		aux->next = node;
	}
	pthread_mutex_unlock(&clientsMutex);	
}
/*
Função removeFromClientsList: Remove a entrada especificada da lista de clientes.
Entrada:
	- node (sClient*) : Entrada a ser removida da lista.
Saída: nenhuma.
*/
void removeFromClientsList(sClient* node)
{
	pthread_mutex_lock(&clientsMutex);
	sClient* aux = clients;
	sClient* prev = NULL;

	while(aux != NULL && aux != node)//Procura o nó correspondente ao do cliente atual na lista encadeada.
	{
		prev = aux;
		aux = aux->next;
	}
	if(aux ==  clients)
		clients = aux->next;

	if(prev != NULL)				
		prev->next = aux->next;

	free(aux);		
	clientsCount--;
	pthread_mutex_unlock(&clientsMutex);
}

/*
Função connectedWorker: Rotina da thread de algum cliente conectado, para recepção de dados.
Entrada: 
	-arg (void*) : Identificador do socket que será utilizado para recepção de dados (int).
Saída: nenhuma.
*/
void* connectedWorker(void* arg)
{
	int conn = (intptr_t)arg;   
    
    BOOL isConnected = TRUE;

	sClient* aux = getClientBySocket(conn);	

	while(running == TRUE && isConnected == TRUE)
	{
		int bytes_recv, i = 1, j = 0;
		char recv_data[1024],substring[1024],buffer[MAX_CHARS * 3],pre[100];

		bytes_recv = recv(conn, recv_data, 1024, 0);
		if(bytes_recv > 0)
		{
			recv_data[bytes_recv] = '\0';		

			while(recv_data[i] != '\0')
			{
				if(recv_data[i] != '\n' && recv_data[i] != '\r')
				{
					substring[j] = recv_data[i];
					j++;				
				}
				i++;
			}		

			substring[j] = '\0';		
		}

		if (bytes_recv <= 0)
		{
			if(onClientClose != NULL)
				onClientClose(aux);

			close(conn);
			isConnected = FALSE;			
			removeFromClientsList(aux);
		}
		else
		{
			if(onPacketReceived != NULL)
				onPacketReceived(aux, recv_data, substring);
		}
	}
}
/*
Função addContact: Adiciona um contato a aprtir de seu IP.
Entrada:
	- ip (char*): IP no formato IPV4.
	- error (int*) : Número do erro ocorrido, caso haja algum( != 1).
Saída: 1 ponteiro de struct sClient, com o cliente adicionado ou NULL, se houve erro.

	Tabela de retornos e respectivos erros

 0: ERRO: Não foi possível criar o socket!
-1: ERRO: Não foi possível definir opções do socket!
-2: ERRO: Não foi possível definir timeout do socket!
-3: ERRO: Não foi possível conectar-se ao contato!
-4: ERRO: Não foi possível criar a thread do contato!
-5: ERRO: IP especificado é inválido!
-6: ERRO: O contato com esse IP já foi adicionado!
*/
sClient* addContact(char* ip, int* error)
{
	int valopt, res, sock, i;
	char send_buffer[1024];	
	long arg;
	fd_set myset; 
  	struct timeval tv; 
  	socklen_t lon;
  	struct timeval timeout;
  	struct sockaddr_in sa;
  	sClient* aux, *client;

    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

  	*error = 1;

  	if(inet_pton(AF_INET, ip, &(sa.sin_addr)) == 0)//Valida o IP.
  	{
  		*error = -5; 
  		return NULL;
  	}

  	pthread_mutex_lock(&clientsMutex);
  	aux = clients;

	while(aux != NULL)//Procura se há algum cliente com o mesmo IP já adicionado.
	{
		if(aux->ip.s_addr == sa.sin_addr.s_addr)		
		{
  			*error = -5; 
  			break;
  		}	
		aux = aux->next;				
	}
	pthread_mutex_unlock(&clientsMutex);

	if(*error == -5)
		return NULL;

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)   	
    {
  		*error = 0; 
  		return NULL;
  	}  	

   	if( (arg = fcntl(sock, F_GETFL, NULL)) < 0)
    {
  		*error = -1; 
  		return NULL;
  	}	

  	arg |= O_NONBLOCK;
	
	//Coloca o soqcket como não bloqueante para o timeout...
  	if( fcntl(sock, F_SETFL, arg) < 0)
    {
  		*error = -1;
  		return NULL;
  	} 		

   	sa.sin_family = AF_INET;
   	sa.sin_port = htons(PORT);   	
   	bzero(&(sa.sin_zero),8);   	

   	res = connect(sock, (struct sockaddr *)&sa, sizeof(struct sockaddr));
   	
   	//Sistema de timeout no socket ao conectar-se com cliente.
   	if (res < 0)
   	{ 
    	if (errno == EINPROGRESS) 
    	{
    		 
        		tv.tv_sec = 5; //Timeout de 5 segundos. 
        		tv.tv_usec = 0; 
        		FD_ZERO(&myset); 
        		FD_SET(sock, &myset); 
        		if (select(sock+1, NULL, &myset, NULL, &tv) > 0) 
        		{ 
        			lon = sizeof(int); 
        			getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon);

        			if (valopt)       				
        			{
  						*error = -3; 
  						return NULL;
  					}     			 
        		} 
        		else 
        		{ 
        			*error = -3; 
  					return NULL;  					 
        		}
        	
     	} 
     	else 
     	{ 
        	*error = -3; 
  			return NULL;  
     	} 
  	} 
	
	//Coloca novamente o socket como bloqueante.
   	if( (arg = fcntl(sock, F_GETFL, NULL)) < 0)  	
    {
    	*error = -3; 
  		return NULL;  
    }

  	arg &= (~O_NONBLOCK); 
  	
  	if( fcntl(sock, F_SETFL, arg) < 0)  	
    {
    	*error = -3; 
  		return NULL;  
    }
	
	//Coloca um timeout de 5 segundos na função "send" do socket.
    if (setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
    {
    	*error = -3; 
  		return NULL;
    }

	client = malloc( sizeof (sClient));

	client->next = NULL;
	strcpy(client->name,"Default");
	client->ip = sa.sin_addr;
	client->connected = sock;
	addOnClientsList(client);

	if(pthread_create(&(client->connectedThread), NULL, (void *)connectedWorker, (void*)(intptr_t)sock) != 0)
	{
		close(sock);		
		removeFromClientsList(client);		
    	*error = -4; 
  		return NULL;  
    
	}			
	return client;
}
/*
Função removeContact: Remove um contato da lista, enviando um pacote de desconexão ao cliente.
Entrada:
	- client (sClient*) : Cliente a ser removido.
Saída: nenhuma.
*/
void removeContact(sClient* client)
{
	char send_buffer[] = "D\0";
	sendPacketToClient(client, send_buffer,strlen(send_buffer));
}
/*
Função listenerdWorker: Rotina da thread de recepção da conexão dos contatos.
Entrada: 
	- arg (void*) : Não utilizada.
Saída: nenhuma.
*/
void* listenerWorker(void* arg)
{
	struct timeval timeout;      
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

	while(running == TRUE)
	{
		sin_size = sizeof(struct sockaddr_in);
		connected = accept(mainSock, (struct sockaddr *)&client_addr, &sin_size);

		if (setsockopt (connected, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0)    	
    		continue;    		

		sClient* client = malloc( sizeof (sClient));
		client->next = NULL;
		client->connected = connected;
		client->ip = client_addr.sin_addr;
		
		addOnClientsList(client);			

		if(pthread_create(&(client->connectedThread), NULL, (void *)connectedWorker, (void*)(intptr_t)connected) != 0)
		{
			close(connected);
			removeFromClientsList(client);
			continue;
		}
		
		if(onClientConnected != NULL)
			onClientConnected(client);			
	}
}

/* Função sendPacketToClient: Envia um pacote a um cliente.
Entradas:
	- client (sClient*) : Cliente para qual o pacote será enviado.
	- packet (char*) : Pacote a ser enviado.
	- packetSize (int) : Tamanho do pacote, em bytes (1 char = 1 byte).	
Saída: 1 inteiro, retornando 1 se houve sucesso no envio e -1 se houve falha.
*/

int sendPacketToClient(sClient* client, char* packet, int packetSize)
{	
	return send(client->connected,packet,packetSize, 0);
}
/*
Função startServer: Inicializa o servidor de recepção de conexões.
Entrada: nenhuma.
Saída: 1 inteiro, informando se foi um sucesso (1) ou erro (0 ou menor).

		Tabela de retornos e respectivos erros

 0: Erro ao iniciar Socket principal
-1:	Erro ao definir opções do Socket principal.
-2: Erro ao realizar bind do socket.
-3:	Erro ao iniciar listen.
-4: Erro ao inicializar thread do Socket Principal.
*/
int startServer()
{
	running = TRUE;	

	if ((mainSock = socket(AF_INET, SOCK_STREAM, 0)) == -1)//Cria o socket principal, que receberá as requisições de contatos.
		return 0;	

	if (setsockopt(mainSock, SOL_SOCKET, SO_REUSEADDR, &ov,sizeof(int)) == -1)//Define opções do socket principal.	
		return -1;		

   	// Configura o endereco de destino
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;
	bzero(&(server_addr.sin_zero),8);

	if (bind(mainSock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1)//Binda o socket principal na porta especificada.	
		return -2;

	if (listen(mainSock, MAX_CLIENTS) == -1)//Inicia a escuta na porta especificada.	
		return -3;	

	if(pthread_create(&listenerThread, NULL, (void *)listenerWorker, NULL) != 0)	
		return -4;

	return 1;
}
/*
Função closeServer: Finaliza o servidor de recepção de conexões.
Entrada: nenhuma.
Saída: nenhuma.
*/
void closeServer()
{
	running = FALSE;
	close(mainSock);
	pthread_cancel(&listenerThread);
	pthread_cancel(&processContactsThread);	
}

/* ----------------------------- FIM DO MÓDULO DO SERVIDOR/CLIENTE ----------------------------- */

/* ----------------------------- INÍCIO DO MÓDULO DA INTERFACE GRÁFICA/PROGRAMA ----------------------------- */
/*
Função logline: Escreve a linha forncida no log em arquivo do programa.
Entrada:
	- line (char*) : Linha a ser escrita no arquivo de log, já com timestamp.
Saída: nenhuma.
*/
void logLine(char* line)
{
	time_t timer;    
    struct tm* tm_info;    

	time(&timer);
    tm_info = localtime(&timer);

    if(openDay != tm_info->tm_yday)
    {
    	if(logfile != NULL)
    		fclose(logfile);

    	openDay = tm_info->tm_yday;

    	char* logFileName = (char*)calloc(150, sizeof(char));
    	sprintf(logFileName, "%sLog %02d-%02d-%02d.txt",LOGFILE_PATH,tm_info->tm_mday, tm_info->tm_mon+1, tm_info->tm_year+1900);
    	logfile = fopen(logFileName, "a");
    	free(logFileName);   	    	
    }
	if(logfile != NULL)
	{
		fwrite(line,sizeof(char), strlen(line), logfile);
		fwrite("\n", sizeof(char), 1, logfile);
	}
}
/*
Função mbstoubs: Função que converte um buffer lido pela função readEntry com suporte a caracteres especiais (2 bytes/caractere) para um buffer com representação de 1 byte/caractere.
Entradas:
	-src (char*) : Vetor que contém os dados lidos pela readEntry no formato de 2 bytes/caractere.
	-dest (char*) : Vetor que recebérá os dados convertidos para o formato de 1 byte/caractere.
Saída: nenhuma.
*/
void mbstoubs(char* src, char* dest)
{
	int i = 0,j = 0;
	while(src[i] != '\0' || src[i+1] != '\0')
	{
		dest[j] = src[i];
		i++;
		if(src[i] < 0)//Caractere especial, ainda necessita de 2 bytes para ser representado, ao contrário dos comuns.
		{
			j++;					
			dest[j] = src[i];
		}		
		i++;
		j++;			
	}
	dest[j] = '\0';
}
/*
Função printLabel: Escreve na tela uma mensagem especificada na janela e posição desejadas.
Entradas:
	-window (WINDOW*) : Janela onde será escrita a mensagem.
	-y (int) : Posição Y do console onde a mensagem será escrita.
	-x (int) : Posição X do console onde a mensagem será escrita.
	-text (char*) : Mensagem a ser escrita.
Saída: nenhuma.
*/
void printLabel(WINDOW *window,int y, int x, char *text)
{
	int maxy, maxx, i;
	getmaxyx(window, maxy, maxx);//Pega o tamanho da janela.	
	for(i = 1; i < (maxx - 1);i++)//Limpa a linha.
		mvwaddch(window,y,i,' ');
	mvwprintw(window,y,x,text);
	touchwin(window);//Refresh da janela.
	wrefresh(window);    
}
/*
Função wCenterTitle: Escreve o título de uma janela no topo dela, centralizado.
Entradas:
	-pwin (WINDOW*) : Janela onde será escrito o título.
	-title (char*) : Título a ser escrito.
	-parent (WINDOW*) : Janela-pai de onde será escrito o título.	
Saída: nenhuma.
*/
void wCenterTitle(WINDOW *pwin, const char * title, WINDOW *parent)
{
	int x, maxy, maxx, stringsize;
	getmaxyx(pwin, maxy, maxx);
	stringsize = 4 + strlen(title);
	x = (maxx - stringsize)/2;
	mvwaddch(pwin, 0, x, ACS_RTEE);//Adiciona um título a janela.
	waddch(pwin, ' ');//Início da escrita do título na janela.
	waddstr(pwin, title);
	waddch(pwin, ' ');
	waddch(pwin, ACS_LTEE);
}
/*
Função wclrscr: Limpa a janela especificada
Entradas:
	-pwin (WINDOW*) : Janela a ser limpa.
Saída: nenhuma.
*/
void wclrscr(WINDOW * pwin)
{
	int y, x, maxy, maxx;
	getmaxyx(pwin, maxy, maxx);
	for(y=0; y < maxy; y++)
		for(x=0; x < maxx; x++)
			mvwaddch(pwin, y, x, ' ');//Preenche a janela com espaços, limpando-a.
}
/*
Função refreshConsole: Reimprime o console, com os dados do buffer do console, na posição especificada.
Entrada: nenhuma.
Saída: nenhuma.
*/
void refreshConsole()
{
	int maxy, maxx, i, pos, start, cnt =0, stop;
	char *text;

	pthread_mutex_lock(&consoleRefreshMutex);
	wclrscr(console_win);
	box(console_win,0,0);
	wCenterTitle(console_win, "Console de Mensagens", base_win);

	if(consoleHead != 0)
	{
		getmaxyx(console_win, maxy, maxx);

		maxy -= 2;//Remoção da posição das bordas.
		maxx -= 2;

		text = (char*)malloc((maxx * 2)*sizeof(char));	

		start = (consoleHead + consolePos)%MAX_LINES;
		stop = start;
			
		while(cnt < maxy)
		{
			start--;
			cnt++;

			if(consoleHead > MAX_LINES)
			{
				if(start < 0)
					start = MAX_LINES - 1;

				if(start == stop)
					break;
			}
			else
			{
				if(start < 0)
				{
					start = 0;
					break;
				}
			}		
		}
		

		for(i = 0;i < cnt; i++)
		{
			if(start+i >= MAX_LINES)		
				start = -i;			
			
			pos = start + i;		
			
			strcpy(text, consoleBuffer[pos]);		
			wattron(console_win, COLOR_PAIR(consoleColor[pos]));
			mvwprintw(console_win,i+1,1,"%s", text);
			wattroff(console_win, COLOR_PAIR(consoleColor[pos]));		
		}

		free(text);
	}

	touchwin(console_win);
	wrefresh(console_win);
	pthread_mutex_unlock(&consoleRefreshMutex);
}
/*
Função scrollConsole: Gerencia o scroll do console, de acordo com o incremento de linha passado.
Entrada:
	- incPosY (int) : Numero de linhas que serão deslocados. Positivo para deslocar para baixo, negativo para deslocar para cima.
Saída: nenhuma.
*/
void scrollConsole(int incPosY)
{
	int maxy, maxx, cnt = 0;
	getmaxyx(console_win, maxy, maxx);

	maxy -= 2;
	maxx -= 2;

	if(consolePos >= 0 && incPosY > 0)
		return;		

	if(consoleHead > MAX_LINES)
	{
		cnt = 30;
	}
	else
	{
		cnt = consoleHead;		
	}	
	if(cnt < maxy && incPosY < 0)
		return;	

	if(consolePos+incPosY >= maxy - cnt)
	{
		consolePos += incPosY;
		refreshConsole();
	}	
}
/*
Função writeOnConsole: Escreve uma linha no console, com tamanho máximo de MAX_CHARS.
Entrada: 
	-line (char*) : Linha a ser escrita no console.
Saída: nenhuma.
*/
void writeOnConsole(char* line, int color)
{
	//Sistema de lista circular, em único vetor.
	int maxy, maxx, size, j = 0, copied,pos;
	getmaxyx(console_win, maxy, maxx);

	maxx -= 2;

	logLine(line);

	pthread_mutex_lock(&consoleWriteMutex);
	size = strlen(line);

	while(size > 0)
	{		
		pos = consoleHead%MAX_LINES;
		strncpy(consoleBuffer[pos], line+j, maxx);
		consoleColor[pos] = color;

		if(consoleBuffer[pos][maxx - 1] < 0 && (maxx - 1)%2 == 0)		
			consoleBuffer[pos][maxx - 1] = '\0';

		copied = strlen(consoleBuffer[pos]);
		size -= copied;
		j += copied;

		consoleHead++;
	}
	pthread_mutex_unlock(&consoleWriteMutex);
	refreshConsole();	
}
/*
Função readEntry: Lê uma sequência de caracteres do teclado, até que ENTER (válido) ou ESC (inválido) sejam apertados.
Entradas: 
	-buffer (char*) : Vetor onde os caracteres lidos serão armazenados.
	-bufferSize (int) : Tamanho do vetor onde os caracteres serão armazenados.
	-startX (int) : Posição X da janela onde o cursor aparecerá, para início da coleta dos caracteres.
	-startY (int) : Posição Y da janela onde o cursor aparecerá, para início da coleta dos caracteres.
	-numericOnly(BOOL) : Indica se a entrada deve conter apenas números e ponto.
	-unicodeSuport(BOOL) : Indica se a entrada deve suportar caracteres especiais (acentuados), utilizando 2 bytes para qualquer caracter no buffer.
Saída: 1 BOOL, indicando se a leitura foi concluída (TRUE - ENTER apertado) ou cancelada (FALSE - ESC apertado).
*/
BOOL readEntry(char* buffer, int bufferSize, int startX, int startY, BOOL numericOnly, BOOL unicodeSupport)
{	
	int specialChar = 0, i = 0, inc = 1, maxx, maxy, baseY, baseX;
	char specialCharBuffer[3], c = ' ';

	if(unicodeSupport == TRUE)
		inc = 2;

	getmaxyx(mainMenu_win, maxy, maxx);

	maxx -= 2;
	maxy -= 2;

	baseY = startY;
	baseX = startX;

	wmove(mainMenu_win,startY,startX+1);
	wrefresh(mainMenu_win);

	while(c != 10)
	{
		if(c == 27)//ESC
			return 0;		

		startY = baseY + (i/inc + baseX)/maxx;		
		startX = 1 + ((i/inc + baseX ) % maxx);		

		c = getch();
		
		touchwin(console_win);
		wrefresh(console_win);		

		if(i >= (bufferSize - 1) && c!= 127 && c != 10)
		{
			wmove(mainMenu_win,startY,startX);
			wrefresh(mainMenu_win);
			continue;
		}
		if(c < 0 && unicodeSupport == TRUE)
		{
			if(specialChar == 0)
				specialChar = c;
			else
			{
				buffer[i] = specialChar;
				specialCharBuffer[0] = specialChar;
				i++;
				buffer[i] = c;
				i++;
				specialCharBuffer[1] = c;
				specialCharBuffer[2] = '\0';
				specialChar = 0;
				mvwprintw(mainMenu_win, startY,startX, "%s", specialCharBuffer);
				wrefresh(mainMenu_win);									
			}
			continue;					
		}


		switch(c)
		{
			case 82://Page Down
				scrollConsole(1);
				break;
			case 83: //Page Up
				scrollConsole(-1);
				break;
			case 10://ENTER
				if(i == 0) //Nenhum outro caractere digitado. Para cancelar, deve digitar ESC e não deixar em branco.
				{
					c = ' ';
					wmove(mainMenu_win,startY,startX);
					wrefresh(mainMenu_win);	
					continue;
				}
				buffer[i] = '\0';
				buffer[i+1] = '\0';
			break;
			case 127://BACKSPACE
				if(i > 0)
				{
					i -= inc;						
					buffer[i] = '\0';

					if(unicodeSupport == TRUE)
						buffer[i+1] ='\0';

					mvwprintw(mainMenu_win,startY,startX - 1," ");
					wmove(mainMenu_win,startY,startX - 1);
					wrefresh(mainMenu_win);
				}
				else
				{
					buffer[i] = '\0';
					if(unicodeSupport == TRUE)
						buffer[i+1] ='\0';
					mvwprintw(mainMenu_win,startY,startX," ");
					wmove(mainMenu_win,startY,startX);
					wrefresh(mainMenu_win);
				}					
			break;
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '.':
				buffer[i] = c;
				if(unicodeSupport == TRUE)
					buffer[i+1] = 0;			
				i += inc;
				wprintw(mainMenu_win, "%c", c);
				wrefresh(mainMenu_win);
			break;			
			default:
				if(numericOnly == FALSE)
				{
					buffer[i] = c;
					if(unicodeSupport == TRUE)
						buffer[i+1] = 0;
					mvwprintw(mainMenu_win, startY,startX,"%c", c);
					i += inc;
								
					wrefresh(mainMenu_win);				
				}
				else
				{
					wmove(mainMenu_win,startY,startX);
					wrefresh(mainMenu_win);
				}
			break;
		}						
	}
	return 1;
}
/*
Função createScrollList: Cria uma lista no formato de scroll.
Entradas:
	-my_menu_win (WINDOW*) : Janela onde a lista será criada.
	-height (int) - Altura do menu (em linhas).
	-width (int) - Largura do menu (em colunas).
	-x (int) - Posição X do menu na janela.
	-y (int) - Posição Y do menu na janela.
	-choices (char**) : Vetor de strings, contendo os itens da lista.
	-desc (char**) : Vetor de strings, contendo os descritores de socket dos contatos.
	-n_choices (int) : Tamanho do vetor de strings 'choices'.
	-chooseEnabled (BOOL) : Ativa ou desativa a escolha de algum item da lista.
	-chooses (int*): Vetor onde os identificadores de socket serão colocados, casos escolhidos, de acordo com a ordem dos itens do menu. Caso não esteja escolhido, o valor será -1. Se vier NULL, apenas 1 item pode ser selecionado.
Saída: 1 inteiro. Se ativado, retorna qual item estava selecionado quando o usuário apertou ENTER. Se pressionar ESC ou não estiver ativada a seleção, retorna -1.
*/
int createScrollList(WINDOW* my_menu_win, int height,int width,int y,int x, char** choices,char** desc, int n_choices, BOOL chooseEnabled, int* chooses)
{
	ITEM **my_items;		
	MENU *my_menu;    
    int i, my_choice = -1, c = 0, choosenCount, maxx,maxy;
    char choosenCountBuffer[100];

    getmaxyx(my_menu_win, maxy,maxx);
	
    my_items = (ITEM **)calloc(n_choices+1, sizeof(ITEM *));
    for(i = 0; i < n_choices; i++)
        my_items[i] = new_item(choices[i], desc[i]);
    my_items[n_choices] = (ITEM*)NULL;
	
	my_menu = new_menu((ITEM **)my_items);

	if(chooses != NULL)
		menu_opts_off(my_menu, O_ONEVALUE);

	keypad(my_menu_win, TRUE);
	curs_set(0);
	
    set_menu_sub(my_menu, derwin(my_menu_win, height-2, width-2, y, x));
    set_menu_format(my_menu, height - y + 1, 1);
	
			
	
    set_menu_mark(my_menu, "* ");
    post_menu(my_menu);
	wrefresh(my_menu_win);	

	while(c != 27)
	{   
		if(chooses != NULL)
		{
			choosenCount = 0;
			for(i = 0; i < item_count(my_menu); i++)
			{
				if(item_value(my_items[i]) == TRUE)
					choosenCount++;
			}				
			sprintf(choosenCountBuffer, "Num. de contatos escolhidos: %i", choosenCount);
			printLabel(my_menu_win,height+1,(maxx - strlen(choosenCountBuffer))/2,choosenCountBuffer);
		}
		c = getch();    
		switch(c)
	        {	case KEY_DOWN:
				menu_driver(my_menu, REQ_DOWN_ITEM);
				break;
			case KEY_UP:
				menu_driver(my_menu, REQ_UP_ITEM);
				break;
			case KEY_NPAGE:
				scrollConsole(1);
				break;
			case ' ':
				if(chooses != NULL)
					menu_driver(my_menu, REQ_TOGGLE_ITEM);
				break;
			case KEY_PPAGE:
				scrollConsole(-1);
				break;
			case 10:
				if(chooseEnabled == TRUE)
				{
					if(chooses != NULL && choosenCount == 0)
						continue;

					my_choice = atoi(item_description(current_item(my_menu)));				
					pos_menu_cursor(my_menu);
					c = 27;
				}			
				break;
			}
        wrefresh(my_menu_win);
	}
	if(chooses != NULL)
	{
		for(i = 0; i < item_count(my_menu); ++i)
		{
			if(item_value(my_items[i]) == TRUE)
				chooses[i] = atoi(item_description(my_items[i]));
			else
				chooses[i] = -1;
		}		
	}
	wrefresh(my_menu_win);
	unpost_menu(my_menu);
    free_menu(my_menu);
    for(i = 0; i <= n_choices; ++i)
        free_item(my_items[i]);
    curs_set(1);

    return my_choice;    
}
/*
Função createMenu: Cria um menu estático e retorna a opção escolhida.
Entradas:
	-wParent (WINDOW*) : Janela-pai de onde o menu será criado.
	-height (int) - Altura do menu (em linhas).
	-width (int) - Largura do menu (em colunas).
	-x (int) - Posição X do menu na janela.
	-y (int) - Posição Y do menu na janela.
	-choices (char**) : Opções do menu.
	-menuName (char*) : Título do menu.
Saída: 1 inteiro, contendo o ID da opção escolhida.
*/
int createMenu(WINDOW *wParent,int height,int width,int y,int x,char* choices[],char* menuName)
{
	ITEM **my_items;	
	MENU *my_menu;
	WINDOW *wUI, *wBorder;
    int c, n_choices, ssChoice, my_choice = -1;

	
	for(n_choices=0; choices[n_choices]; n_choices++);//Calcula o número de opções.

	//Aloca cada opção do menu.
	my_items = (ITEM **)calloc(n_choices + 1, sizeof(ITEM *));

	for(ssChoice = 0; ssChoice < n_choices; ssChoice++)
		my_items[ssChoice] = new_item(choices[ssChoice], NULL);

	my_items[n_choices] = (ITEM *)NULL;
	
	my_menu = new_menu((ITEM **)my_items);

	set_menu_mark(my_menu, "> ");

	//Borda do menu.
	wBorder = mainMenu_win;
	wclrscr(mainMenu_win);
	box(mainMenu_win,0,0);
	wCenterTitle(wBorder, menuName, wParent);
	touchwin(wBorder);
	wrefresh(wBorder);

	//Janela para interface do menu.
	wUI = derwin(wBorder, height-2, width-2, y, x);

	//Associa as janelas com o menu criado.
	set_menu_win(my_menu, wBorder);
	set_menu_sub(my_menu, wUI);	

	keypad(wUI, TRUE);	//Ativa a detecção de teclas.
	noecho();		//Não printa teclas aperttadas pelo usuário.
	curs_set(0);		//Não mostra o cursor intermitente.
	
	post_menu(my_menu);
	
	touchwin(wBorder);
	wrefresh(wBorder);  

	//Gerencia as opções.
	while(my_choice == -1)
	{
		touchwin(wUI);	
		wrefresh(wUI); 	
		c = getch();
		switch(c)
		{
			case KEY_DOWN:
				menu_driver(my_menu, REQ_DOWN_ITEM);
			break;
			case KEY_UP:
				menu_driver(my_menu, REQ_UP_ITEM);
			break;
			case KEY_NPAGE: //Scroll do Console de Mensagens.
				scrollConsole(1);
			break;
			case KEY_PPAGE: //Scroll do Console de Mensagens.
				scrollConsole(-1);
			break;
			case 10:	// ENTER
				my_choice = item_index(current_item(my_menu));				
				pos_menu_cursor(my_menu);
			break;			
		}
	}	

	//Liberando os recursos alocados.
	unpost_menu(my_menu);
	for(ssChoice = 0; ssChoice <= n_choices; ssChoice++)
		free_item(my_items[ssChoice]);

	free(my_items);	
	free_menu(my_menu);

	//Destruindo a janela alocada para o menu.
	delwin(wUI);
	
	curs_set(1);			//Faz o cursor intermitente ser visível outra vez.
	
	touchwin(wParent);
	wrefresh(wParent);
	
	return my_choice;
}
/*
Função resetMainMenu: Limpa a janela do Menu Principal e atribui um novo título.
Entrada: 
	-title (char*) : Novo título da janela.
Saída: nenhuma.
*/
void resetMainMenu(char* title)
{
	wclrscr(mainMenu_win);
	box(mainMenu_win,0,0);
	wCenterTitle(mainMenu_win, title, base_win);
	touchwin(mainMenu_win);
	wrefresh(mainMenu_win);
}
/*
Função processContactsWorker: Rotina da thread de sincronização dos contatos recebidos.
Entrada: 
	- arg (void*) : Não utilizada.
Saída: nenhuma.
*/
void* processContactsWorker(void* arg)
{
	char* ip;
	sClient* aux, *client;
	int result;	

	while(running == TRUE)
	{
		sem_wait(&processContacts);		
		pthread_mutex_lock(&processingContactsMutex);//Inicia zona crítica da lista de contatos para processar.
		while(contactsToProcess != NULL)//Verifica se o contato passado é ele mesmo. Se for, não adiciona.
		{
			if(contactsToProcess->ip.s_addr == contactsToProcess->ipRcv.s_addr || clientsCount >= MAX_CLIENTS)
			{
				contactsToProcess = contactsToProcess->next;
				continue;
			}

			ip = inet_ntoa(contactsToProcess->ip);

			client = addContact(ip, &result);			
			aux = contactsToProcess;
			contactsToProcess = contactsToProcess->next;
			free(aux);				
		}
		pthread_mutex_unlock(&processingContactsMutex);//termina zona crítica da lista de contatos para processar.
	}
}
/*
Função enviaContatos: envia o IP dos contatos contidos na lista encadeada de clientes.
Entradas:
	- client (sClient*) : Cliente para qual os contatos serão enviados.
	- packetType (char) : Caractere inicial do pacote de contatos ("J" ou "H").
	
Saída: nenhuma.
*/ 
void enviaContatos(sClient* client, char packetType)
{
	int count = 1;
	char contactsBuffer[(MAX_CLIENTS+2) * 4 + 2];
	sClient* aux = clients;

	if(clientsCount == 0)
		return;
	pthread_mutex_lock(&clientsMutex);
	contactsBuffer[0] = packetType;

	memcpy(contactsBuffer+1, &(client->ip.s_addr), 4);
	memcpy(contactsBuffer+5, &clientsCount, 4);

	while(aux != NULL)
	{
		memcpy(contactsBuffer+5+(count*4), &(aux->ip.s_addr), 4);
		count++;
		aux = aux->next;
	}
	pthread_mutex_unlock(&clientsMutex);

	sendPacketToClient(client, contactsBuffer, (count + 2) * 4 + 2);
}
/*
Função adicionarContato: Função que gerencia a adição de um contato.
Entrada: nenhuma.
Saída: nenhuma.
*/
void adicionarContato()
{
	int ok=0, maxy, maxx, i, result;
	char IP [16],c, send_buffer[MAX_NAME*2 + 2];
	sClient* client;

   	resetMainMenu("Adicionar contato");
	getmaxyx(mainMenu_win, maxy, maxx);

	if(clientsCount >= MAX_CLIENTS)
	{
		printLabel(mainMenu_win,maxy - 3,(maxx - 47)/2,"ERRO: O número máximo de contatos foi atingido.");
     	printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
     	getch();
     	return;
    }
	
	cbreak();//Desativa o buffer de linha, passando todos os caracteres pressionados instântaneamente.
	keypad(stdscr, TRUE);	//Ativa detecção de teclas.

	while(ok == FALSE)//Loop para coletar e validar o IP.
	{
		c = ' ';
		i = 0;
		ok = TRUE;
		curs_set(1);//Mostra o cursor intermitente.
		printLabel(mainMenu_win,2,3,"Digite o IP do contato:");
		printLabel(mainMenu_win,4,3,"Pressione ESC para voltar ao menu principal.");
		wmove(mainMenu_win,2,26);//Coloca o cursor após os dois pontos.

		if(readEntry(IP, 16, 26, 2, TRUE, FALSE) == 0)
			return;

		curs_set(0);		
		noecho();

		printLabel(mainMenu_win,maxy - 2,(maxx - 27)/2,"Conectando-se ao contato...");
		client = addContact(IP, &result);

		if(result < -4)
		{
			switch(result)
			{
				case -5:
					printLabel(mainMenu_win,maxy - 2,(maxx - 33)/2,"ERRO: IP especificado é inválido!");
				break;
				case -6:
					printLabel(mainMenu_win,maxy - 2,(maxx - 46)/2,"ERRO: O contato com esse IP já foi adicionado!");
				break;
			}
			ok = FALSE;
		}
    	
	}	

	if(result != 1)
	{
		switch(result)
		{
			case 0:
				printLabel(mainMenu_win,maxy - 3,(maxx - 38)/2,"ERRO: Não foi possível criar o socket!");
			break;
			case -1:
				printLabel(mainMenu_win,maxy - 3,(maxx - 47)/2,"ERRO: Não foi possível definir opções do socket!");
			break;
			case -2:
				printLabel(mainMenu_win,maxy - 3,(maxx - 50)/2,"ERRO: Não foi possível definir timeout do socket!");
			break;
			case -3:
				printLabel(mainMenu_win,maxy - 3,(maxx - 46)/2,"ERRO: Não foi possível conectar-se ao contato!");
			break;
			case -4:
				printLabel(mainMenu_win,maxy - 3,(maxx - 49)/2,"ERRO: Não foi possível criar a thread do contato!");
			break;
		}
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();
	}   
}
/*
Função listarContatos: Função que lista os contatos adicionados.
Entrada: nenhuma.
Saída: nenhuma.
*/
void listarContatos()
{
	int maxx,maxy,i,size,max;	
	char *ip, ** itens, **desc;;
	sClient* aux;

	getmaxyx(mainMenu_win, maxy, maxx);

	resetMainMenu("Lista de contatos");

	if(clients == NULL)
	{
		printLabel(mainMenu_win,maxy/2,(maxx - 26)/2,"Nenhum contato adicionado!");
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();
	}
	else
	{
		printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");
		aux = clients;

		size = clientsCount;

		itens = (char**)malloc(sizeof(char*) * size);
		desc = (char**)malloc(sizeof(char*) * 4);		
		i = 0;
		max = 0;

		while(aux != NULL)
		{
			ip = inet_ntoa(aux->ip);
			itens[i] = (char*)calloc(sizeof(char),((strlen(aux->name) + 30)));
			desc[i] = (char*)calloc(sizeof(char),4);

			sprintf(itens[i],"%s@%s",aux->name,ip);
			sprintf(desc[i], "%d", aux->connected);

			if((strlen(itens[i]) + 4) > max)
				max = strlen(itens[i]) + 4;	
			i++;
			aux = aux->next;			
		}

		createScrollList(mainMenu_win, (maxy - 4), max, 2,(maxx - max)/2,itens,desc, size, FALSE, NULL);

		for(i = 0;i < size; i++)
		{
			free(itens[i]);
			free(desc[i]);
		}

		free(itens);
	}	
}
/*
Função removeContato: Função que remove um contato selecionado.
Entrada: nenhuma.
Saída: nenhuma.
*/
void removeContato()
{
	int maxx,maxy,i,chosen,size,max = 0;	       		 
	sClient* aux;
	char *ip, **itens, **desc;

	resetMainMenu("Remover contato");	
	getmaxyx(mainMenu_win, maxy, maxx);	

	if(clients == NULL)
	{
		printLabel(mainMenu_win,maxy/2,(maxx - 26)/2,"Nenhum contato adicionado!");
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();
	}
	else
	{
		printLabel(mainMenu_win,1,(maxx - 33)/2,"Escolha um contato para remover: ");
		printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");

		aux = clients;
		size = clientsCount;

		itens = (char**)malloc(sizeof(char*) * size);
		desc = (char**)malloc(sizeof(char*) * size);		
		i = 0;

		while(aux != NULL)
		{
			ip = inet_ntoa(aux->ip);
			itens[i] = (char*)calloc(sizeof(char),((strlen(aux->name) + 30)));
			desc[i] = (char*)calloc(sizeof(char),4);			
			sprintf(desc[i], "%d", aux->connected);
			sprintf(itens[i],"%s@%s",aux->name,ip);
			if(strlen(itens[i]) + 4 > max)
				max = strlen(itens[i]) + 4;
			i++;
			aux = aux->next;			
		}
		
		chosen = createScrollList(mainMenu_win, maxy - 5, max, 2,(maxx - max)/2, itens, desc, clientsCount, TRUE, NULL);

		for(i = 0;i < clientsCount; i++)
		{
			free(itens[i]);
			free(desc[i]);
		}

		free(itens);

		if(chosen == -1)
			return;		

		aux = getClientBySocket(chosen);

		if(aux != NULL)
		{
			removeContact(aux);					
			printLabel(mainMenu_win,maxy - 3,(maxx - 17)/2,"Contato removido!");
		}
		else
		{
			printLabel(mainMenu_win,maxy - 3,(maxx - 17)/2,"ERRO: O contato já foi removido!");
		}
		
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();		

	}	
}
/*
Função enviaMensagemGrupo: Função que envia uma mensagem a um contato selecionado.
Entrada: nenhuma.
Saída: nenhuma.
*/
void enviaMensagemGrupo()
{
	char buffer[MAX_NAME * 2 + 50], messageBuffer[MAX_CHARS * 2 + 2], send_buffer[1024],buffer2[MAX_CHARS * 3],pre[100], errorMessage[150];
	char* ip;
	int maxx,maxy,i,chosen,size,choosenCount=0,max=0;	
	BOOL messageSent, fail;
	struct timespec tv;
	time_t timer;    
    struct tm* tm_info;        
    sClient* aux;
    int* chooses;
    char** itens, **desc;

	resetMainMenu("Enviar mensagem em grupo");

	getmaxyx(mainMenu_win, maxy, maxx);	

	if(clients == NULL)
	{
		printLabel(mainMenu_win,maxy/2,(maxx - 26)/2,"Nenhum contato adicionado!");
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();
	}
	else
	{
		printLabel(mainMenu_win,1,(maxx - 44)/2,"Escolha os contatos para enviar a mensagem: ");
		printLabel(mainMenu_win,maxy - 4,(maxx - 41)/2,"Pressione ENTER para finalizar a seleção.");		
		printLabel(mainMenu_win,maxy - 3,(maxx - 44)/2,"Pressione ESPACO para selecionar um contato.");
		printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");
		aux = clients;
		size = clientsCount;

		itens = (char**)malloc(sizeof(char*) * size);
		desc = (char**)malloc(sizeof(char*) * size);

		chooses = (int*)malloc(sizeof(int) * size);		
		i = 0;

		while(aux != NULL)
		{
			ip = inet_ntoa(aux->ip);
			itens[i] = (char*)calloc(sizeof(char),((strlen(aux->name) + 30)));
			desc[i] = (char*)calloc(sizeof(char),4);
			sprintf(itens[i], "%s@%s",aux->name,ip);
			sprintf(desc[i], "%d", aux->connected);
			
			if(strlen(itens[i]) + 4 > max)
				max = strlen(itens[i]) + 4;
			
			i++;
			aux = aux->next;			
		}
		
		chosen = createScrollList(mainMenu_win, (maxy) - 6, max, 2,(maxx - max)/2, itens, desc, size, TRUE, chooses);

		if(chosen == -1)
			return;

		for(i = 0;i < size; i++)
		{
			if(chooses[i] == -1)
				continue;
			choosenCount++;
		}		

		sprintf(buffer, "Num. de contato(s) escolhido(s): %i", choosenCount);
		messageSent = FALSE;
		fail = FALSE;

		while(TRUE)
		{
			resetMainMenu("Enviar mensagem privada");
				
			if(messageSent == TRUE)
			{
				printLabel(mainMenu_win,maxy - 4,(maxx - 25)/2,"Mensagem enviada a todos!");
				messageSent = FALSE;
			}
			
			printLabel(mainMenu_win,2,2, buffer);
			printLabel(mainMenu_win,3,2,"Digite sua mensagem: ");
			printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");

			if(readEntry(messageBuffer, MAX_CHARS * 2, 22, 3, FALSE, TRUE) == FALSE)		
					return;

			printLabel(mainMenu_win,maxy - 4,(maxx - 20)/2,"Enviando mensagem...");

			char* temp = (char*)calloc(sizeof(char), 1024);
			mbstoubs(messageBuffer, temp);
			sprintf(send_buffer, "C%s", temp);

			for(i = 0;i < size; i++)
			{
				if(chooses[i] == -1)
					continue;

				aux = getClientBySocket(chooses[i]);

				if(aux != NULL)
				{
					gettimeofday(&tv, NULL);
	    			tv.tv_sec = tv.tv_sec + 5;//Timeout de 5 segundos para a recepção da mensagem de confirmação.


					if(sendPacketToClient(aux, send_buffer,strlen(send_buffer)) == -1 || sem_timedwait(&messageReceived, &tv) == -1)
					{
						fail = TRUE;						
						sprintf(errorMessage, "ERRO: Falha ao enviar mensagem ao contato %s@%s!", aux->name, inet_ntoa(aux->ip));
						printLabel(mainMenu_win,maxy - 4,(maxx - strlen(errorMessage))/2,errorMessage);
						printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
						getch();
						break;
					}
				}
				else
				{
					fail = TRUE;
					sprintf(errorMessage, "ERRO: O contato %s está desconectado!", itens[i]);
					printLabel(mainMenu_win,maxy - 4,(maxx - strlen(errorMessage))/2,errorMessage);
					printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
					getch();
					break;
				}				
			}

			if(fail == TRUE)
			{
				free(temp);			
				break;
			}

			messageSent = TRUE;

			time(&timer);
	    	tm_info = localtime(&timer);
			strftime(buffer2, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(GRP) Para vários: ");
			strcat(buffer2, pre);
			strcat(buffer2, temp);
			writeOnConsole(buffer2, BLUEONBLACK);

			free(temp);			
		}

		for(i = 0;i < size; i++)
		{
			free(itens[i]);	
			free(desc[i]);	
		}		
		free(itens);
		free(desc);		
	}	
}
/*
Função enviaMensagemPrivada: Função que envia uma mensagem a um contato selecionado.
Entrada: nenhuma.
Saída: nenhuma.
*/
void enviaMensagemPrivada()
{
	char buffer[MAX_NAME * 2 + 50], messageBuffer[MAX_CHARS * 2 + 2], send_buffer[1024],buffer2[MAX_CHARS * 3],pre[100];
	char* ip, **itens, **desc;
	int maxx,maxy,i,chosen,size,max=0;	
	BOOL messageSent;
	struct timespec tv;
	time_t timer;    
    struct tm* tm_info;
    sClient* aux;        		 

	resetMainMenu("Enviar mensagem privada");	
	getmaxyx(mainMenu_win, maxy, maxx);	

	if(clients == NULL)
	{
		printLabel(mainMenu_win,maxy/2,(maxx - 26)/2,"Nenhum contato adicionado!");
		printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
		getch();
	}
	else
	{
		printLabel(mainMenu_win,1,(maxx - 43)/2,"Escolha um contato para enviar a mensagem: ");
		printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");

		aux = clients;
		size = clientsCount;
		
		itens = (char**)malloc(sizeof(char*) * size);	
		desc = (char**)malloc(sizeof(char*) * size);	
		i = 0;

		while(aux != NULL)
		{
			ip = inet_ntoa(aux->ip);
			itens[i] = (char*)calloc(sizeof(char),((strlen(aux->name) + 30)));
			desc[i] = (char*)calloc(sizeof(char),4);
			sprintf(itens[i], "%s@%s",aux->name,ip);
			sprintf(desc[i], "%d", aux->connected);
			if(strlen(itens[i]) + 4 > max)
				max = strlen(itens[i]) + 4;
			i++;
			aux = aux->next;			
		}
		
		chosen = createScrollList(mainMenu_win, (maxy) - 4, max, 2,(maxx - max)/2, itens, desc, clientsCount, TRUE, NULL);

		for(i = 0;i < clientsCount; i++)
		{
			free(itens[i]);
			free(desc[i]);
		}

		free(itens);
		free(desc);

		if(chosen == -1)
			return;

		aux = getClientBySocket(chosen);

		if(aux != NULL)
		{
			messageSent = FALSE;
			sprintf(buffer, "Contato escolhido: %s@%s", aux->name, inet_ntoa(aux->ip));
			
			while(TRUE)
			{
				resetMainMenu("Enviar mensagem privada");
				
				if(messageSent == TRUE)
				{
					printLabel(mainMenu_win,maxy - 4,(maxx - 17)/2,"Mensagem enviada!");
					messageSent = FALSE;
				}
				printLabel(mainMenu_win,2,2, buffer);
				printLabel(mainMenu_win,3,2,"Digite sua mensagem: ");
				printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");

				if(readEntry(messageBuffer, MAX_CHARS * 2, 22, 3, FALSE, TRUE) == FALSE)		
					return;

				printLabel(mainMenu_win,maxy - 4,(maxx - 20)/2,"Enviando mensagem...");

				char* temp = (char*)calloc(sizeof(char), 1024);
				mbstoubs(messageBuffer, temp);
				sprintf(send_buffer, "B%s", temp);

				gettimeofday(&tv, NULL);
	    		tv.tv_sec = tv.tv_sec + 5;//Timeout de 5 segundos para a recepção da mensagem de confirmação.
		
				if(sendPacketToClient(aux, send_buffer,strlen(send_buffer)) == -1 || sem_timedwait(&messageReceived, &tv) == -1)
				{
					printLabel(mainMenu_win,maxy - 4,(maxx - 31)/2,"ERRO: Falha ao enviar mensagem!");
					printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
					getch();
					break;
				}				

				messageSent = TRUE;

				time(&timer);
	    		tm_info = localtime(&timer);
				strftime(buffer2, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
				sprintf(pre, "(PVT) Para %s@%s: ", aux->name, ip);
				strcat(buffer2, pre);
				strcat(buffer2, temp);
				writeOnConsole(buffer2, BLUEONBLACK);

				free(temp);
			}
		}
		else
		{
			printLabel(mainMenu_win,maxy - 4,(maxx - 31)/2,"ERRO: O contato escolhido está desconectado!");
			printLabel(mainMenu_win,maxy - 2,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
			getch();			
		}		
	}	
}
/*
Função trocarNome: Função que permite a troca do nome de exibição do cliente.
Entrada: nenhuma.
Saída: nenhuma.
*/
void trocarNome()
{
	char send_buffer[1024], old[MAX_NAME*2], buffer[400],pre[200],*temp;
	time_t timer;    
    struct tm* tm_info;
	int maxx,maxy,i;

	resetMainMenu("Alterar nome");
	
	getmaxyx(mainMenu_win, maxy, maxx);

	temp = (char*)calloc(sizeof(char), MAX_NAME * 2 + 2);

	while(strlen(temp) == 0)
	{
		printLabel(mainMenu_win,maxy - 2,(maxx - 44)/2,"Pressione ESC para voltar ao menu principal.");
		printLabel(mainMenu_win,2,2,"Digite seu novo nome: ");
		if(readEntry(temp, MAX_NAME * 2, 24, 2, FALSE, TRUE) == FALSE)		
			return;		
	}

	strcpy(old, myName);
	mbstoubs(temp,myName);

	free(temp);
	time(&timer);
    tm_info = localtime(&timer);
	strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
	sprintf(pre, "Nome alterado de \"%s\" para \"%s\"", old, myName);
	strcat(buffer, pre);					
	writeOnConsole(buffer, REDONBLACK);

	sprintf(send_buffer, "E%s", myName);
	sClient* aux = clients;

	while(aux != NULL)
	{
		sendPacketToClient(aux, send_buffer,strlen(send_buffer));		
		aux = aux->next;
	}	
}
/*
Função exibeAjuda: Função que mosta as teclas de para controle do programa.
Entrada: nenhuma.
Saída: nenhuma.
*/
void exibeAjuda()
{
	int maxx,maxy,i;	

	resetMainMenu("Ajuda");
	
	getmaxyx(mainMenu_win, maxy, maxx);

	maxy -= 2;
	maxx -= 2;

	printLabel(mainMenu_win,2,2,"PAGE UP: Sobe uma linha no Console de Mensagens.");
	printLabel(mainMenu_win,3,2,"PAGE DOWN: Desce uma linha no Console de Mensagens.");
	printLabel(mainMenu_win,4,2,"SETA P/ CIMA (ARROW UP): Sobe uma opção no menu.");
	printLabel(mainMenu_win,5,2,"SETA P/ BAIXO (ARROW DOWN): Desce uma opção no menu.");
	printLabel(mainMenu_win,6,2,"ENTER: Seleciona opção do menu ou termina entrada do usuário.");
	printLabel(mainMenu_win,7,2,"ESPACO: Seleciona opção do menu de múltipla seleção (Msg em Grupo).");
	printLabel(mainMenu_win,8,2,"ESC: Cancela a ação atual.");
	printLabel(mainMenu_win,maxy,(maxx - 55)/2,"Pressione qualquer tecla para voltar ao menu principal.");
	getch();	

}
/*
Função guiPacketReceived: Função da GUI chamada pelo servidor quando um pacote é recebido.
Entradas:
	- client (sClient*) : Cliente que recebeu o pacote.
	- recv_data (char*) : Pacote cru recebido.
	- substring (char*) : Pacote com o primeiro caractere removido e \n no término.
Saída: nenhuma.
*/
void guiPacketReceived(sClient* client, char* recv_data, char* substring)
{
	time_t timer;    
    struct tm* tm_info;    
	char buffer[MAX_CHARS * 3],pre[100], *ip, old[MAX_NAME + 1], messageConfirmation[] = "F\0", requestContacts[] = "G\0",send_buffer[1024];
	
	int count = 0,i,myIp, aIp;
	sClient* aux, *node;

	ip = inet_ntoa(client->ip);
	
	switch(recv_data[0])
	{
		case 'A': //Contato passa seu nome para quem chamou a conexão.
			strcpy(client->name,substring);
			time(&timer);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(Contato adicionado): %s@%s", client->name, ip);
			strcat(buffer, pre);					
			writeOnConsole(buffer, REDONBLACK);
			sprintf(send_buffer, "I%s", myName);
			sendPacketToClient(client, send_buffer,strlen(send_buffer));								
		break;
		case 'B': //Mensagem privada
			time(&timer);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(PVT) De %s@%s: ", client->name, ip);
			strcat(buffer, pre);
			strcat(buffer,substring);
			writeOnConsole(buffer, GREENONBLACK);
			sendPacketToClient(client, messageConfirmation, 2);//Confirmação de mensagem recebida.
		break;
		case 'C': //Mensagem em grupo
			time(&timer);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(GRP) De %s@%s: ", client->name, ip);
			strcat(buffer, pre);
			strcat(buffer,substring);
			writeOnConsole(buffer, CYANONBLACK);
			sendPacketToClient(client, messageConfirmation, 2);//Confirmação de mensagem recebida.
		break;
		case 'D': //Contato enviou desconexão.
			close(client->connected);		
		break;
		case 'E': //Contato renomeado					
			strcpy(old, client->name);
			strcpy(client->name,substring);
			time(&timer);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(Contato renomeado): %s@%s para %s@%s", old, ip, client->name, ip);
			strcat(buffer, pre);					
			writeOnConsole(buffer, REDONBLACK);
		break;
		case 'F'://Confirmação de mensagem recebida.
			sem_post(&messageReceived);
		break;
		case 'G'://Solicita lista de contatos.
			enviaContatos(client, 'H');
		break;
		case 'H'://Recebe contatos de quem conectou.
			memcpy(&myIp, recv_data+1, 4); //IP do cliente solicitante, para evitar de adicionar a si mesmo.			
			memcpy(&count, recv_data+5, 4); //Número de contatos recebidos.			
			pthread_mutex_lock(&processingContactsMutex);//Inicia zona crítica da lista de contatos para processar.
			for(i = 1; i <= count; i++)
			{
				node = malloc( sizeof (sClient));
				node->next = NULL;
				memcpy(&aIp, recv_data+5+(i*4), 4);								
				node->ip.s_addr = aIp;
				node->ipRcv.s_addr = myIp;

				aux = contactsToProcess;	

				if(contactsToProcess == NULL)
				{
					contactsToProcess = node;	
				}
				else
				{
					while(aux->next != NULL)
						aux = aux->next;
					aux->next = node;
				}
			}			
			pthread_mutex_unlock(&processingContactsMutex);//Fim zona crítica da lista de contatos para processar.
			enviaContatos(client, 'J');
			sem_post(&processContacts);			
		break;
		case 'I': //Contato passa seu nome para quem recebeu a conexão.
			strcpy(client->name,substring);
			time(&timer);
			tm_info = localtime(&timer);
			strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
			sprintf(pre, "(Contato adicionado): %s@%s", client->name, ip);
			strcat(buffer, pre);					
			writeOnConsole(buffer, REDONBLACK);	
			sendPacketToClient(client, requestContacts, 2);								
		break;
		case 'J'://Recebe contatos de quem recebeu a conexão.
			memcpy(&myIp, recv_data+1, 4); //IP do cliente solicitante, para evitar de adicionar a si mesmo.			
			memcpy(&count, recv_data+5, 4); //Número de contatos recebidos.			
			pthread_mutex_lock(&processingContactsMutex);//Inicia zona crítica da lista de contatos para processar.
			for(i = 1; i <= count; i++)
			{
				node = malloc( sizeof (sClient));
				node->next = NULL;
				memcpy(&aIp, recv_data+5+(i*4), 4);								
				node->ip.s_addr = aIp;
				node->ipRcv.s_addr = myIp;

				aux = contactsToProcess;	

				if(contactsToProcess == NULL)
				{
					contactsToProcess = node;	
				}
				else
				{
					while(aux->next != NULL)
						aux = aux->next;
					aux->next = node;
				}
			}
			pthread_mutex_unlock(&processingContactsMutex);//Fim zona crítica da lista de contatos para processar.
			sem_post(&processContacts);			
		break;
	}
}
/*
Função guiClientClose: Função da GUI chamada pelo servidor quando um cliente fecha a conexão.
Entrada:
	- client (sClient*) : Cliente que desconectou.
Saída: nenhuma.
*/
void guiClientClose(sClient* client)
{
	time_t timer;    
    struct tm* tm_info;
    char buffer[MAX_CHARS * 3],pre[100];      

	time(&timer);
    tm_info = localtime(&timer);
	strftime(buffer, 26, "(%d/%m/%Y %H:%M:%S)", tm_info);
	sprintf(pre, "(Contato desconectado): %s@%s", client->name, inet_ntoa(client->ip));
	strcat(buffer, pre);					
	writeOnConsole(buffer, REDONBLACK);
}
/*
Função guiClientConnected: Função da GUI chamada pelo servidor quando um cliente é conectado.
Entrada:
	- client (sClient*) : Cliente que conectou.
Saída: nenhuma.
*/
void guiClientConnected(sClient* client)
{
	char send_buffer[1024];
	sprintf(send_buffer, "A%s", myName);//Envia o nome para quem conectou
	sendPacketToClient(client, send_buffer,strlen(send_buffer));		
}
/*
Função cleanUP: Função que limpa as alocações feitas pela biblioteca ncurses e pelo resto do programa, para finalizá-lo.
Entrada: nenhuma.
Saída: nenhuma.
*/
void cleanUp()
{
	closeServer();	

	if(mainMenu_win != NULL)
		delwin(mainMenu_win);
	if(base_win != NULL)
		delwin(base_win);
	if(console_win != NULL)
		delwin(console_win);
	if(logfile != NULL)
    		fclose(logfile);

	endwin();//Finaliza todas as janelas do ncurses.	
}
/*
Função fatalErrorMessage: Imprime uma mensagem de erro fatal, saindo do programa assim que o usuário pressionar qualquer tecla.
Entrada:
	-message (char*) : Mensagem de erro a ser informada ao usuário.
Saída: nenhuma.
*/
void fatalErrorMessage(char* message)
{
	int maxx,maxy;
	getmaxyx(mainMenu_win, maxy, maxx);
	printLabel(mainMenu_win,maxy/2,(maxx - strlen(message))/2,message);		
	printLabel(mainMenu_win,maxy/2 + 1,(maxx - 51)/2,"Pressione qualquer tecla para finalizar o programa.");
	getch();	
	cleanUp();
	exit(1);
}

void main()
{
	int i,maxx,maxy,y,result,n,stringsize,x;
	char *temp, *mainMenu[] = {"1. Adicionar contato","2. Lista de contatos","3. Remover contato","4. Enviar mensagem","5. Enviar mensagem em grupo","6. Alterar nome","7. Ajuda", "8. Sair", NULL};

	getch();
	setlocale(LC_ALL, "Portuguese");

	sem_init(&messageReceived, 0, 0);//Semáforo para confirmação de mensagem recebida.
	sem_init(&processContacts, 0, 0);//Semáforo para liberar execução da thread de sincronização de contatos.

	//signal(SIGWINCH, onResize);//Sinaliza um resize da janela.
	//inicializando curses 
	initscr();
	cbreak();	
    noecho();		//Não printa teclas digitadas pelo usuário.
	keypad(stdscr, TRUE);	//Ativa detecção de teclas.

	getmaxyx(stdscr, maxy, maxx);//Pega o tamanho da janela do console.
	start_color();//Inicia as cores
	init_pair(1, COLOR_WHITE,COLOR_BLACK);
	init_pair(2, COLOR_RED,COLOR_BLACK);
	init_pair(3, COLOR_GREEN,COLOR_BLACK);
	init_pair(4, COLOR_BLUE,COLOR_BLACK);
	init_pair(5, COLOR_CYAN,COLOR_BLACK);
	wrefresh(stdscr);//Faz um refresh no console.

	
	//Alocando janela principal
	base_win = newwin(maxy, maxx, 0,0);	
	wattrset(base_win, WA_BOLD);	
	wclrscr(base_win);
	box(base_win, 0, 0);
	wCenterTitle(base_win, "Chatsapp", stdscr);	
	touchwin(base_win);
	refresh();
	wrefresh(base_win);	

	//Alocando janela do menu principal.
	mainMenu_win = newwin(maxy/2, maxx - 4, 1,2);
	wattrset(mainMenu_win, WA_BOLD);
	wclrscr(mainMenu_win);	
	box(mainMenu_win, 0, 0);
	wCenterTitle(mainMenu_win, "Menu principal", base_win);	
	touchwin(mainMenu_win);
	wrefresh(mainMenu_win);

	//Alocando janela do console.
	console_win = newwin(maxy*0.45, maxx - 4, maxy/2 + 1,2);
	wattrset(console_win, WA_BOLD);
	wclrscr(console_win);		
	box(console_win, 0, 0);
	wCenterTitle(console_win, "Console de Mensagens", base_win);	
	touchwin(console_win);
	wrefresh(console_win);	

	getmaxyx(mainMenu_win, y, i);	

	temp = (char*)calloc(sizeof(char), MAX_NAME * 2 + 2);

	while(strlen(temp) == 0)
	{
		printLabel(mainMenu_win,y - 2,(maxx - 40)/2,"Pressione ESC para finalizar o programa.");
		printLabel(mainMenu_win,2,2,"Digite seu nome: ");
		if(readEntry(temp, MAX_NAME * 2, 18, 2, FALSE, TRUE) == FALSE)
		{
			cleanUp();
			exit(1);
		}
	}

	mbstoubs(temp,myName);	
	free(temp);

	//Registra eventos de recepção de novo cliente, recepção de pacote e cliente desconectado.

	onClientConnected = &guiClientConnected;
	onPacketReceived = &guiPacketReceived;
	onClientClose = &guiClientClose;

	result = startServer();

	if(result != 1)
	{
		switch(result)
		{
			case 0:
				fatalErrorMessage("Erro ao iniciar Socket principal. Tente abrir o programa novamente.");				
			break;
			case -1:
				fatalErrorMessage("Erro ao definir opções do Socket principal. Tente abrir o programa novamente.");	
			break;
			case -2:
				fatalErrorMessage("Erro ao realizar bind do socket.");
			break;
			case -3:
				fatalErrorMessage("Erro ao iniciar listen. Tente abrir o programa novamente.");	
			break;
			case -4:
				fatalErrorMessage("Erro ao inicializar thread do Socket Principal. Tente abrir o programa novamente.");	
			break;
		}
	}

	if(pthread_create(&processContactsThread, NULL, (void *)processContactsWorker, NULL) != 0)	
		fatalErrorMessage("Erro ao inicializar thread de sincronia de contatos. Tente abrir o programa novamente.");

	stringsize = 4 + strlen("Menu principal");
	x = (maxx - 33)/2;	
	
	while(n != 7)
	{
		n = createMenu(mainMenu_win, (maxy/2) -2, 40, 2,x,mainMenu, "Menu principal");		
		
		while(n<0 || n>7)		
			n = createMenu(mainMenu_win, (maxy/2) - 2, 40, 2,x,mainMenu, "Menu principal");		
		
		switch(n)
		{
			case 0:
				adicionarContato();
			break;
			case 1:
				listarContatos();
			break;
			case 2:
				removeContato();
			break;
			case 3:
				enviaMensagemPrivada();
			break;
			case 4:
				enviaMensagemGrupo();
			break;
			case 5:
				trocarNome();
			break;
			case 6:
				exibeAjuda();
			break;
		}
	}

	cleanUp();	
}
/* ----------------------------- FIM DO MÓDULO DA INTERFACE GRÁFICA/PROGRAMA ----------------------------- */