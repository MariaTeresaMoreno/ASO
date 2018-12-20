/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: GUERRERO MUNUERA, MARÍA DOLORES (G1.2)
 *          MORENO BOLUDA, MARÍA TERESA (G1.2)
 *
 * Convocatoria: FEBRERO
 */


/*
 * Ficheros de cabecera
 */
#define _POSIX_C_SOURCE 200809L /* IEEE 1003.1-2008 (véase /usr/include/features.h) */
//#define NDEBUG                /* Traduce asertos y DMACROS a 'no ops' */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <libgen.h>
#include <unistd.h> 
#include <sys/param.h>
#include <signal.h>

// Biblioteca readline
#include <readline/readline.h>
#include <readline/history.h>
#include <regex.h>

/******************************************************************************
 * Constantes, macros y variables globales
 ******************************************************************************/


static const char* VERSION = "0.18";

// Niveles de depuración
#define DBG_CMD   (1 << 0)
#define DBG_TRACE (1 << 1)
// . . .
static int g_dbg_level = 0;

#ifndef NDEBUG
#define DPRINTF(dbg_level, fmt, ...)                            \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            fprintf(stderr, "%s:%d:%s(): " fmt,                 \
                    __FILE__, __LINE__, __func__, ##__VA_ARGS__);       \
    } while ( 0 )

#define DBLOCK(dbg_level, block)                                \
    do {                                                        \
        if (dbg_level & g_dbg_level)                            \
            block;                                              \
    } while( 0 );
#else
#define DPRINTF(dbg_level, fmt, ...)
#define DBLOCK(dbg_level, block)
#endif

#define TRY(x)                                                  \
    do {                                                        \
        int __rc = (x);                                         \
        if( __rc < 0 ) {                                        \
            fprintf(stderr, "%s:%d:%s: TRY(%s) failed\n",       \
                    __FILE__, __LINE__, __func__, #x);          \
            fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",     \
                    __rc, errno, strerror(errno));              \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while( 0 )


// Número máximo de argumentos de un comando
#define MAX_ARGS 16

//max procesos
#define MAX_PROCESS 8

static int secondProcess[MAX_PROCESS]; //DONDE GUARDO LOS PROCESOS EN SEGUNDO PLANO
static sigset_t signalChld;
int status = 0; //estado del proceso ejecutandose

struct cmd* cmd;
struct cmd* cmd1;
// Delimitadores
static const char WHITESPACE[] = " \t\r\n\v";
// Caracteres especiales
static const char SYMBOLS[] = "<|>&;()";


/******************************************************************************
 * Funciones auxiliares
 ******************************************************************************/


// Imprime el mensaje
void info(const char *fmt, ...)
{
    va_list arg;

    fprintf(stdout, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stdout, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error
void error(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
}


// Imprime el mensaje de error y aborta la ejecución
void panic(const char *fmt, ...)
{
    va_list arg;

    fprintf(stderr, "%s: ", __FILE__);
    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    exit(EXIT_FAILURE);
}


// `fork()` que muestra un mensaje de error si no se puede crear el hijo
int fork_or_panic(const char* s)
{
    int pid;

    pid = fork();
    if(pid == -1)
        panic("%s failed: errno %d (%s)", s, errno, strerror(errno));
    return pid;
}


/******************************************************************************
 * Estructuras de datos `cmd`
 ******************************************************************************/


// Las estructuras `cmd` se utilizan para almacenar información que servirá a
// simplesh para ejecutar líneas de órdenes con redirecciones, tuberías, listas
// de comandos y tareas en segundo plano. El formato es el siguiente:

//     |----------+--------------+--------------|
//     | (1 byte) | ...          | ...          |
//     |----------+--------------+--------------|
//     | type     | otros campos | otros campos |
//     |----------+--------------+--------------|

// Nótese cómo las estructuras `cmd` comparten el primer campo `type` para
// identificar su tipo. A partir de él se obtiene un tipo derivado a través de
// *casting* forzado de tipo. Se consigue así polimorfismo básico en C.

// Valores del campo `type` de las estructuras de datos `cmd`
enum cmd_type { EXEC=1, REDR=2, PIPE=3, LIST=4, BACK=5, SUBS=6, INV=7 };

struct cmd { enum cmd_type type; };

// Comando con sus parámetros
struct execcmd {
    enum cmd_type type;
    char* argv[MAX_ARGS];
    char* eargv[MAX_ARGS];
    int argc;
};

// Comando con redirección
struct redrcmd {
    enum cmd_type type;
    struct cmd* cmd;
    char* file;
    char* efile;
    int flags;
    mode_t mode;
    int fd;
};

// Comandos con tubería
struct pipecmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Lista de órdenes
struct listcmd {
    enum cmd_type type;
    struct cmd* left;
    struct cmd* right;
};

// Tarea en segundo plano (background) con `&`
struct backcmd {
    enum cmd_type type;
    struct cmd* cmd;
};

// Subshell
struct subscmd {
    enum cmd_type type;
    struct cmd* cmd;
};


/******************************************************************************
 * Funciones para construir las estructuras de datos `cmd`
 ******************************************************************************/


// Construye una estructura `cmd` de tipo `EXEC`
struct cmd* execcmd(void)
{
    struct execcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("execcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `REDR`
struct cmd* redrcmd(struct cmd* subcmd,
        char* file, char* efile,
        int flags, mode_t mode, int fd)
{
    struct redrcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("redrcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->flags = flags;
    cmd->mode = mode;
    cmd->fd = fd;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `PIPE`
struct cmd* pipecmd(struct cmd* left, struct cmd* right)
{
    struct pipecmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("pipecmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*) cmd;
}

// Construye una estructura `cmd` de tipo `LIST`
struct cmd* listcmd(struct cmd* left, struct cmd* right)
{
    struct listcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("listcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `BACK`
struct cmd* backcmd(struct cmd* subcmd)
{
    struct backcmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("backcmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;

    return (struct cmd*)cmd;
}

// Construye una estructura `cmd` de tipo `SUB`
struct cmd* subscmd(struct cmd* subcmd)
{
    struct subscmd* cmd;

    if ((cmd = malloc(sizeof(*cmd))) == NULL)
    {
        perror("subscmd: malloc");
        exit(EXIT_FAILURE);
    }
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = SUBS;
    cmd->cmd = subcmd;

    return (struct cmd*) cmd;
}


/******************************************************************************
 * Funciones para realizar el análisis sintáctico de la línea de órdenes
 ******************************************************************************/


// `get_token` recibe un puntero al principio de una cadena (`start_of_str`),
// otro puntero al final de esa cadena (`end_of_str`) y, opcionalmente, dos
// punteros para guardar el principio y el final del token, respectivamente.
//
// `get_token` devuelve un *token* de la cadena de entrada.

int get_token(char** start_of_str, char* end_of_str,
        char** start_of_token, char** end_of_token)
{
    char* s;
    int ret;

    // Salta los espacios en blanco
    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // `start_of_token` apunta al principio del argumento (si no es NULL)
    if (start_of_token)
        *start_of_token = s;

    ret = *s;
    switch (*s)
    {
        case 0:
            break;
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            s++;
            break;
        case '>':
            s++;
            if (*s == '>')
            {
                ret = '+';
                s++;
            }
            break;

        default:

            // El caso por defecto (cuando no hay caracteres especiales) es el
            // de un argumento de un comando. `get_token` devuelve el valor
            // `'a'`, `start_of_token` apunta al argumento (si no es `NULL`),
            // `end_of_token` apunta al final del argumento (si no es `NULL`) y
            // `start_of_str` avanza hasta que salta todos los espacios
            // *después* del argumento. Por ejemplo:
            //
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //     | (espacio) | a | r | g | u | m | e | n | t | o | (espacio)
            //     |
            //     |-----------+---+---+---+---+---+---+---+---+---+-----------|
            //                   ^                                   ^
            //            start_o|f_token                       end_o|f_token

            ret = 'a';
            while (s < end_of_str &&
                    !strchr(WHITESPACE, *s) &&
                    !strchr(SYMBOLS, *s))
                s++;
            break;
    }

    // `end_of_token` apunta al final del argumento (si no es `NULL`)
    if (end_of_token)
        *end_of_token = s;

    // Salta los espacios en blanco
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;

    // Actualiza `start_of_str`
    *start_of_str = s;

    return ret;
}


// `peek` recibe un puntero al principio de una cadena (`start_of_str`), otro
// puntero al final de esa cadena (`end_of_str`) y un conjunto de caracteres
// (`delimiter`).
//
// El primer puntero pasado como parámero (`start_of_str`) avanza hasta el
// primer carácter que no está en el conjunto de caracteres `WHITESPACE`.
//
// `peek` devuelve un valor distinto de `NULL` si encuentra alguno de los
// caracteres en `delimiter` justo después de los caracteres en `WHITESPACE`.

int peek(char** start_of_str, char* end_of_str, char* delimiter)
{
    char* s;

    s = *start_of_str;
    while (s < end_of_str && strchr(WHITESPACE, *s))
        s++;
    *start_of_str = s;

    return *s && strchr(delimiter, *s);
}


// Definiciones adelantadas de funciones
struct cmd* parse_line(char**, char*);
struct cmd* parse_pipe(char**, char*);
struct cmd* parse_exec(char**, char*);
struct cmd* parse_subs(char**, char*);
struct cmd* parse_redr(struct cmd*, char**, char*);
struct cmd* null_terminate(struct cmd*);
void run_cmd(struct cmd* cmd);
void free_cmd(struct cmd* cmd);
void freeMemory();
void bloqueoSIGCHLD();
void desbloqueoSIGCHLD();


// `parse_cmd` realiza el *análisis sintáctico* de la línea de órdenes
// introducida por el usuario.
//
// `parse_cmd` utiliza `parse_line` para obtener una estructura `cmd`.

struct cmd* parse_cmd(char* start_of_str)
{
    char* end_of_str;
    struct cmd* cmd;

    DPRINTF(DBG_TRACE, "STR\n");

    end_of_str = start_of_str + strlen(start_of_str);

    cmd = parse_line(&start_of_str, end_of_str);

    // Comprueba que se ha alcanzado el final de la línea de órdenes
    peek(&start_of_str, end_of_str, "");
    if (start_of_str != end_of_str)
        error("%s: error sintáctico: %s\n", __func__);

    DPRINTF(DBG_TRACE, "END\n");

    return cmd;
}


// `parse_line` realiza el análisis sintáctico de la línea de órdenes
// introducida por el usuario.
//
// `parse_line` comprueba en primer lugar si la línea contiene alguna tubería.
// Para ello `parse_line` llama a `parse_pipe` que a su vez verifica si hay
// bloques de órdenes y/o redirecciones.  A continuación, `parse_line`
// comprueba si la ejecución de la línea se realiza en segundo plano (con `&`)
// o si la línea de órdenes contiene una lista de órdenes (con `;`).

struct cmd* parse_line(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_pipe(start_of_str, end_of_str);

    while (peek(start_of_str, end_of_str, "&"))
    {
        // Consume el delimitador de tarea en segundo plano
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '&');

        // Construye el `cmd` para la tarea en segundo plano
        cmd = backcmd(cmd);
    }

    if (peek(start_of_str, end_of_str, ";"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de lista de órdenes
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == ';');

        // Construye el `cmd` para la lista
        cmd = listcmd(cmd, parse_line(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_pipe` realiza el análisis sintáctico de una tubería de manera
// recursiva si encuentra el delimitador de tuberías '|'.
//
// `parse_pipe` llama a `parse_exec` y `parse_pipe` de manera recursiva para
// realizar el análisis sintáctico de todos los componentes de la tubería.

struct cmd* parse_pipe(char** start_of_str, char* end_of_str)
{
    struct cmd* cmd;
    int delimiter;

    cmd = parse_exec(start_of_str, end_of_str);

    if (peek(start_of_str, end_of_str, "|"))
    {
        if (cmd->type == EXEC && ((struct execcmd*) cmd)->argv[0] == 0)
            error("%s: error sintáctico: no se encontró comando\n", __func__);

        // Consume el delimitador de tubería
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '|');

        // Construye el `cmd` para la tubería
        cmd = pipecmd(cmd, parse_pipe(start_of_str, end_of_str));
    }

    return cmd;
}


// `parse_exec` realiza el análisis sintáctico de un comando a no ser que la
// expresión comience por un paréntesis, en cuyo caso se llama a `parse_subs`.
//
// `parse_exec` reconoce las redirecciones antes y después del comando.

struct cmd* parse_exec(char** start_of_str, char* end_of_str)
{
    char* start_of_token;
    char* end_of_token;
    int token, argc;
    struct execcmd* cmd;
    struct cmd* ret;

    // ¿Inicio de un bloque?
    if (peek(start_of_str, end_of_str, "("))
        return parse_subs(start_of_str, end_of_str);

    // Si no, lo primero que hay en una línea de órdenes es un comando

    // Construye el `cmd` para el comando
    ret = execcmd();
    cmd = (struct execcmd*) ret;

    // ¿Redirecciones antes del comando?
    ret = parse_redr(ret, start_of_str, end_of_str);

    // Bucle para separar los argumentos de las posibles redirecciones
    argc = 0;
    while (!peek(start_of_str, end_of_str, "|)&;"))
    {
        if ((token = get_token(start_of_str, end_of_str,
                        &start_of_token, &end_of_token)) == 0)
            break;

        // El siguiente token debe ser un argumento porque el bucle
        // para en los delimitadores
        if (token != 'a')
            error("%s: error sintáctico: se esperaba un argumento\n", __func__);

        // Almacena el siguiente argumento reconocido. El primero es
        // el comando
        cmd->argv[argc] = start_of_token;
        cmd->eargv[argc] = end_of_token;
        cmd->argc = ++argc;
        if (argc >= MAX_ARGS)
            panic("%s: demasiados argumentos\n", __func__);

        // ¿Redirecciones después del comando?
        ret = parse_redr(ret, start_of_str, end_of_str);
    }

    // El comando no tiene más parámetros
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;

    return ret;
}


// `parse_subs` realiza el análisis sintáctico de un bloque de órdenes
// delimitadas por paréntesis o `subshell` llamando a `parse_line`.
//
// `parse_subs` reconoce las redirecciones después del bloque de órdenes.

struct cmd* parse_subs(char** start_of_str, char* end_of_str)
{
    int delimiter;
    struct cmd* cmd;
    struct cmd* scmd;

    // Consume el paréntesis de apertura
    if (!peek(start_of_str, end_of_str, "("))
        error("%s: error sintáctico: se esperaba '('", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == '(');

    // Realiza el análisis sintáctico hasta el paréntesis de cierre
    scmd = parse_line(start_of_str, end_of_str);

    // Construye el `cmd` para el bloque de órdenes
    cmd = subscmd(scmd);

    // Consume el paréntesis de cierre
    if (!peek(start_of_str, end_of_str, ")"))
        error("%s: error sintáctico: se esperaba ')'", __func__);
    delimiter = get_token(start_of_str, end_of_str, 0, 0);
    assert(delimiter == ')');

    // ¿Redirecciones después del bloque de órdenes?
    cmd = parse_redr(cmd, start_of_str, end_of_str);

    return cmd;
}


// `parse_redr` realiza el análisis sintáctico de órdenes con
// redirecciones si encuentra alguno de los delimitadores de
// redirección ('<' o '>').

struct cmd* parse_redr(struct cmd* cmd, char** start_of_str, char* end_of_str)
{
    int delimiter;
    char* start_of_token;
    char* end_of_token;

    // Si lo siguiente que hay a continuación es delimitador de
    // redirección...
    while (peek(start_of_str, end_of_str, "<>"))
    {
        // Consume el delimitador de redirección
        delimiter = get_token(start_of_str, end_of_str, 0, 0);
        assert(delimiter == '<' || delimiter == '>' || delimiter == '+');

        // El siguiente token tiene que ser el nombre del fichero de la
        // redirección entre `start_of_token` y `end_of_token`.
        if ('a' != get_token(start_of_str, end_of_str, &start_of_token, &end_of_token))
            error("%s: error sintáctico: se esperaba un fichero", __func__);

        // Construye el `cmd` para la redirección
        switch(delimiter)
        {
            case '<':
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDONLY, 0, 0);
                break;
            case '>': //O_RDWR
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU, 1);
                break;
            case '+': // >>
                cmd = redrcmd(cmd, start_of_token, end_of_token, O_RDWR|O_CREAT|O_APPEND, S_IRWXU, 1);
                break;
        }
    }

    return cmd;
}


// Termina en NULL todas las cadenas de las estructuras `cmd`
struct cmd* null_terminate(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct pipecmd* pcmd;
    struct listcmd* lcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int i;

    if(cmd == 0)
        return 0;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            for(i = 0; ecmd->argv[i]; i++)
                *ecmd->eargv[i] = 0;
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            null_terminate(rcmd->cmd);
            *rcmd->efile = 0;
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            null_terminate(pcmd->left);
            null_terminate(pcmd->right);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            null_terminate(lcmd->left);
            null_terminate(lcmd->right);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            null_terminate(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            null_terminate(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    return cmd;
}


/******************************************************************************
 * Funciones para la ejecución de la línea de órdenes
 ******************************************************************************/


void exec_cmd(struct execcmd* ecmd)
{
    assert(ecmd->type == EXEC);

    if (ecmd->argv[0] == 0) exit(EXIT_SUCCESS);

    execvp(ecmd->argv[0], ecmd->argv);

    panic("no se encontró el comando '%s'\n", ecmd->argv[0]);
}

/* run_exit llama a la función freeMemory encargada de liberar la memoria de la estructura
cmd y finaliza simplesh*/
void run_exit(){
    //En la ejecución de un exit desde el comando src se tiene que liberar la propia estrucutra del exit junto con la de src
    if(cmd1 != NULL){
	free_cmd(cmd1);
        free(cmd1);
    }
    freeMemory();
    exit(EXIT_SUCCESS);
}

// get_path() obtiene el directorio actual (ruta absoluta)
char* get_path(){
    char *ruta = malloc(PATH_MAX);
    if(!ruta){
        perror("getcwd: malloc");
        exit(EXIT_FAILURE);
    }
    getcwd(ruta, PATH_MAX);
    if (!ruta){
       perror("getcwd");
        exit(EXIT_FAILURE); 
    }

    return ruta;
}

//run_cwd() obtiene la ruta absoluta del directorio actual
void run_cwd(){
    char *ruta = get_path();
    //comprobar si la ruta es valida
    if(!ruta)
        fprintf(stderr,"simplesh: cwd: ");
    
    fprintf(stdout,"cwd: %s\n", ruta);
    free(ruta);
}

/*run_cd() ejecuta el comando cd directorio moviéndonos al directorio que se indica como argumento
    cd " " nos sitúa en el directorio HOME
    cd - nos sitúa en el directorio anterior OLDPWD*/
void run_cd(struct execcmd* cmd){ 

    char *old = get_path();
    
    //Se ejecuta cd sin argumentos
    if(!cmd->argv[1]){
      if(chdir(getenv("HOME")) == -1)
         fprintf(stderr,"run_cd: Variable HOME no está definida\n");
    }else if(strcmp(cmd->argv[1], "-") == 0){
    //Se ejecuta cd -
       if(chdir(getenv("OLDPWD")) == -1)
            fprintf(stderr,"run_cd: Variable OLDPWD no definida\n");
    }else{
    //Ejecución de cd ruta  
       if(chdir(cmd->argv[1]) == -1)
         fprintf(stderr,"run_cd: No existe el directorio '%s'\n", cmd->argv[1]);
    }
    //cd dir si hay más argumentos salta la excepción
    if(cmd->argc > 2){ 
        fprintf(stderr,"run_cd: Demasiados argumentos\n");
        //exit(EXIT_FAILURE);
    }
    //Actualización de variable de entorno OLDPWD
    if(setenv("OLDPWD", old, 1) == -1)
        fprintf(stderr, "run_cd: No se ha podido actualizar la variable de entorno.");
    //liberar old
    free(old);
}

//Lectura de la entrada de datos, ya sea fichero o entrada estándar, y escritura en base
//a la cantidad de bytes solicitada o la cantidad de líneas
void parse_data(int file_descriptor, int tsize, int lsize, int bsize){
   int n_lineas = 0;
   int n_bytes = 0;
   char buffer[tsize];
   char buffer_aux[tsize];
   int readed, aux_size, index_b;
   memset(buffer, 0, tsize);	
	while((readed = read(file_descriptor, buffer, tsize))>0){			
	//Impresión de las líneas solicitadas con el parámetro -t
	if (lsize != -1){
	  index_b = 0;
	  //Cuenta la cantidad de líneas que contiene el buffer
	  while(n_lineas < lsize && index_b < readed){
		if (buffer[index_b] == '\n' )
		  n_lineas ++;
		index_b ++;
	  }
	  //Imprime las líneas del búffer
	  write(STDOUT_FILENO, buffer, index_b);
	  if(n_lineas == lsize) return;
	}
	//Procesamiento de impresión de bytes
	if(bsize != -1){
	//En caso de que la cantidad de bytes impresos y la cantidad de bytes leídos hasta el momento sea inferior a los que se solicitan los mostrará
	if((n_bytes + readed)<= bsize){
	     write(STDOUT_FILENO, buffer, readed);
	     n_bytes += readed;
	  }else{
		//En caso de que sea superior muestra la cantidad que falte para completar bsize
	    aux_size = bsize-n_bytes;
	     write(STDOUT_FILENO, buffer, aux_size);
	  }

	}
	
   }	  
}
//Ejecución del comando hd, para ello se requiere el comando completo pasado por parámetro
//Se llevará a cabo internamente los tratamientos de error y la ejecución del mismo.
void run_hd(struct execcmd * cmd){
	    int opt,  lsize, bsize, tsize;
	    optind = 1;
	    tsize = 1024;
	    lsize = -1; 
	    bsize = -1;	
	    int file_descriptor;
			
	//Caso de solicitud de parámetro -h
	if(cmd->argc > 1){
		if (strcmp(cmd->argv[1], "-h") == 0){	
			//Si hay más de dos argumentos no se ha construido bien el comando. Debe ser: hd -h		
			if(cmd->argc > 2){
				fprintf(stderr, "hd: Opción no válida\n");
				return;
			}   
			//Impresión de la ayuda del comando, se ha introducido correctamente el: hd -h
			fprintf(stdout, "Uso: hd [-l NLINES] [-b NBYTES] [-t BSIZE] [FILE1] [FILE2] ...\n\tOpciones:\n\t-l NLINES Número máximo de líneas a mostrar.\n\t-b NBYTES Número máximo de bytes a mostrar.\n\t-t BSIZE Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n\t-h help\n");
			return;
		}

	}
	//Procesamiento del comando con las posibles opciones
	    while ((opt = getopt(cmd->argc, cmd->argv, "l:b:t:h")) != -1) {
		switch (opt) {
		    case 'l':
			lsize = atoi(optarg);	
			//En caso de que el tamaño de líneas a leer sea negativo, no es válido. 
			if(lsize <= 0){
			   fprintf(stderr, "hd: Opción no válida\n");
			   break;		
			}
			if ( bsize != -1){
			     fprintf(stderr, "hd: Opciones incompatibles\n");
  			    return;
			}
		       break;
		    case 'b':
			bsize = atoi(optarg);
		       //En caso de que el tamaño de bytes a leer sea negativo, no es válido. 
			if(bsize <= 0){
			   fprintf(stderr, "hd: Opción no válida\n");
			   return;		
			}
			if ( lsize != -1){
			     fprintf(stderr, "hd: Opciones incompatibles\n");
  			    return;
			}
			break;
		    case 't':
			tsize = atoi(optarg);
			//En caso de que el tamaño de líneas a leer sea negativo, no es válido. 
			if(tsize <= 0){
			   fprintf(stderr, "hd: Opción no válida\n");
			   return;		
			}
			break;
		    default:
			fprintf(stderr, "Usage: %s [-f] [-n NUM]\n", cmd->argv[0]);
		}
	    }
	//Si no se ha dado un valor de lsize ni de bsize quiere decir que se solicta 
	if(lsize == -1 && bsize == -1) lsize = 3;
	//El parámetro no consta de ficheros que leer, así que la información proviene de la entrada estándar
	if(optind == cmd->argc){    
	   parse_data(STDIN_FILENO, tsize, lsize, bsize); 
	}else{
		//Recorre los ficheros del comando para ejecutar en cada uno de ellos el comando
		for(int i = optind; i < cmd->argc; i++){
			file_descriptor = open(cmd->argv[i], O_RDONLY);
			if(file_descriptor != -1){
				parse_data(file_descriptor, tsize, lsize, bsize);
				close(file_descriptor);			
			}else{	
			   fprintf(stderr, "hd: No se encontró el archivo '%s'\n", cmd->argv[i]);					
			}
		}
	}	
}

//test_delimiter se encarga de comprobar que la opción indicada en el comando src para el parámetro -d sea válida
//para que lo sea, el único requisito es que se trate de un solo caracter de separación. 
int test_delimiter(char * delim){
 if(strlen(delim) == 1) return 0;
 return 1;
}
//Función auxiliar encargada de llevar a cabo la ejecución de un comando reconocido por src
void exec_src_cmd(char * line){
	//struct cmd* cmd1;
	cmd1 =  parse_cmd(line);
	null_terminate(cmd1);
	run_cmd(cmd1);
	free_cmd(cmd1);
        free(cmd1);
}
//Procesamiento de los datos obtenidos por parte del comando src, ya sea de entrada estándar o desde fichero
//Se encarga de obtener los comandos y ejecutarlos
void parse_source(int file_descriptor, int tsize, char delim){
   int n_lineas;
   char buffer[tsize];
   char * token, ptrInicio, ptrFinal;
   int readed, index_b;
   int offset = 0;
   memset(buffer, 0, tsize);
   //La lectura se realiza teniendo en cuenta un offset. Este offset será distinto de cero siempre y cuando al llenar el buffer en una lectura, se haya leído una línea a mitad
   //Puede darse el caso en que el final del buffer contenga por ejemplo "echo " y que el resto del comando se obtenga en la siguiente lectura, con lo cual no podrá procesarse correctamente
   while((readed = read(file_descriptor, buffer+offset, tsize-offset))>0){		
		n_lineas = 0;
		readed = readed+offset;
		//En caso de que se complete el buffer de lectura que comprobará si se ha llevado a cabo una lectura parcial de una línea o no
		if(readed == tsize){
			offset = readed;
			//Posiciona el offset en el último final de línea del buffer
			while(buffer[offset] != '\n'){
			    offset--;
			}
			offset++;
			index_b = 0;
			//Cuenta la cantidad de líneas completas
			while(index_b< readed){
			    if(buffer[index_b] == '\n') n_lineas++;
			    index_b++;
			}
			//Pasa a procesar las líneas 
			token = strtok(buffer, "\n");
			//En caso de que la cantidad de líneas sea cero, se ha encontrado una línea que no ha finalizado en "\n" 
			//ya que se ha realizado una lectura válida pero no ha encontrado el final de 				
			//ninguna línea.
			if(n_lineas == 0){
	  		   ptrInicio = *buffer;	
			   ptrFinal = *(buffer+readed-1);
			   //Comprueba que no se trate de un comentario, si no lo es llama a la función auxiliar de procesamiento		   
			   if(ptrInicio != delim && ptrFinal != delim ){
				exec_src_cmd(buffer);
			   }
			}
			//Procesa las líneas que ha contabilizado sin tener en cuenta el anterior caso especial
			while(token != NULL && n_lineas > 0){
				   ptrInicio = *token;	
				   ptrFinal = *(token+strlen(token)-2);
				   //Comprueba que no sea un comentario y se ejecuta invocando a la función auxiliar				   		   
				   if(ptrInicio != delim && ptrFinal != delim ){
					exec_src_cmd(token);
				   }
				token = strtok(NULL, "\n");
				n_lineas--;
			}
		      	//Mueve la línea parcial leída al inicio del buffer y posiciona el offset en la posición a partir de la cual se deberá de empezar a escribir en
			//la siguiente lectura la información obtenida con read()
			memmove(buffer, buffer+offset, offset);
			offset = readed - offset;	
		}else{
			//Se procesa el buffer sin tener en cuenta líneas parciales, ya que no las contendrá porque no se ha llenado completamente
			n_lineas = 0;
			index_b=0;
			//Contabilización de líneas leídas
			while(index_b< readed){
			    if(buffer[index_b] == '\n') n_lineas++;
			    index_b++;
			}
			token = strtok(buffer, "\n");
			//Tratamiento del caso especial de línea sin \n
			if(n_lineas == 0){
	  		   ptrInicio = *buffer;	
			   ptrFinal = *(buffer+readed-1);		   
			   if(ptrInicio != delim && ptrFinal != delim ){
				exec_src_cmd(buffer);
			   }
			}
			//Tratamiento de líneas contabilizadas 
			while(token != NULL && n_lineas > 0){
				   ptrInicio = *token;	
				   ptrFinal = *(token+strlen(token)-1);	
				//Comprueba que no sea un comentario y se ejecuta invocando a la función auxiliar		   
				   if(ptrInicio != delim && ptrFinal != delim ){
					exec_src_cmd(token);
				   }
				token = strtok(NULL, "\n");
				n_lineas--;
			}
			//Limpia el buffer de lectura para que, en caso de leer una cantidad menor a la actualmente leída, no se corrompan los datos.
			memset(buffer, 0, tsize);
			//Dado que no hay líneas parciales leídas, el valor del offset pasará a ser cero.
			offset = 0;
		} 
			   	
  	}	  
}

//Ejecución del comando interno src. Para ello, se lleva a cabo el tratamiento de los distintos parámetros disponibles.
void run_src(struct execcmd *cmd){
	    int opt, tsize;
	    optind = 0;
	    tsize = 4096;
	    int file_descriptor;
	    char delim = '%';
	//Procesamiento del comando con las posibles opciones
	    while ((opt = getopt(cmd->argc, cmd->argv, "hd:h")) != -1) {
		switch (opt) {
		    case 'd':
			//Comprobación de que el delimitador de comentario solicitado es válido y de que no se da el caso en que se indiquen varios de ellos.
			delim = optarg[0];
			if(test_delimiter(optarg)){
				fprintf(stdout, "src: Opción no válida\n");
				return;
			}
			if(cmd->argc > 3){
			     if(!test_delimiter(optarg) && !test_delimiter(cmd->argv[optind])){
  				   fprintf(stdout, "src: Opción no válida\n");
				   return;
			      }
			}
		       break;
		    case 'h':
			//Solicitud de ayuda del comando, comprueba que se ejecuta de la forma adecuada: src -h y no contiene ningún argumento adicional.
			if ((cmd->argc - optind) == 0){
			     fprintf(stdout, "Uso: src [-d DELIM] [FILE1] [FILE2]...\n\tOpciones:\n\t-d DELIM Carácter de inicio de comentarios.\n\t-h help\n");

			     return;
			}else {
			     fprintf(stdout, "src: Opción no válida\n");
			     return;
			}	
			break;
		    default:
			fprintf(stderr, "Usage: %s [-f] [-n NUM]\n", cmd->argv[0]);
		}
	    }
	if(cmd->argc == optind){
		//Reconocimiento de los comandos por pantalla 
		 parse_source(STDIN_FILENO, tsize, delim);
	}else{
		//Recorre los ficheros disponibles para ejecutar las órdenes encontradas en el listado de ficheros introducidos
		for(int i = optind; i < cmd->argc; i++){
			//Comprueba que el descriptor de fichero exista lo que nos indicará que el fichero existe.
			if((file_descriptor = open(cmd->argv[i], O_RDONLY)) != -1){
				//Procesamiento del contenido de los ficheros
				parse_source(file_descriptor, tsize, delim);			
				close(file_descriptor);
			}else{
			   fprintf(stdout, "src: No se encontró el archivo '%s'\n", cmd->argv[i]);
			}
			
		}

	}
}

void run_bjobs(struct execcmd* cmd){

	optind = 1;
	int option = 0;
	/* flag=0 indica bjobs -c
		flag=1 indica bjobs -s*/
	int flag =0;
	//bjobs sin argumento
	if(cmd->argc == 1){
		//imprimo la lista de procesos en segundo plano
		for (int i = 0; i < MAX_PROCESS; ++i)
			if(secondProcess[i] != 0)
				fprintf(stdout, "[%d]\n", secondProcess[i]);
	}else{
		while ((option=getopt(cmd->argc,cmd->argv,"hsc")) != -1) {
			switch (option) {
				case 'h': //help
					fprintf(stdout, "Uso: bjobs [-s] [-c] [-h]\n\tOpciones:\n\t-s Suspende todos los procesos en segundo plano.\n\t-c Reanuda todos los procesos en segundo plano.\n\t-h help\n");
				break;

				case 's': //suspende todos los procesos
					flag = 1;
				break;

				case 'c': //reanuda todos los procesos
					//trata el bjobs -s -c
					if(flag==1){
						fprintf(stderr, "bjobs: Opciones incompatibles\n");
						return; //hago que salga de la función para que no se ejecute -s
					}else
						for (int i = 0; i< MAX_PROCESS; ++i){
							if(secondProcess[i] != 0){
								if(kill(secondProcess[i],SIGCONT) == -1){
									perror("run_bjobs -> kill + SIGCONT");
									exit(EXIT_FAILURE);
								}
								//elimino el pid del proceso que se ha reanudado
								secondProcess[i] = 0;
							}
						}
				break;
			}
		}
		if(flag==1){
			for (int i = 0; i < MAX_PROCESS; i++){
				if(secondProcess[i] != 0){
					if(kill(secondProcess[i],SIGSTOP) == -1){
									perror("run_bjobs -> kill + SIGSTOP");
									exit(EXIT_FAILURE);
								}
				}
			}
		}
	}

}

// `checkCommand` comprueba si un comando es interno o no.
// Devuelve 0 si es comando interno, 1 en caso contrario.
int checkCommand(struct execcmd* cmd){
    if (cmd->argv[0] == NULL)
        return 0;
   
    if (strcmp(cmd->argv[0],"cwd") == 0){
        run_cwd();
        return 0;
    }
   
    if (strcmp(cmd->argv[0],"exit") == 0){
        run_exit();
        return 0;
    }
   
    if (strcmp(cmd->argv[0],"cd") == 0){
        run_cd(cmd);
        return 0;
    }
   if (strcmp(cmd->argv[0],"hd") == 0){
        run_hd(cmd);
        return 0;
    }
   if (strcmp(cmd->argv[0],"src") == 0){
        run_src(cmd);
        return 0;
    }
    if(strcmp(cmd->argv[0],"bjobs")==0){
    	run_bjobs(cmd);
    	return 0;
    }
    return 1;
}

void free_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            free_cmd(rcmd->cmd);

            free(rcmd->cmd);
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;

            free_cmd(lcmd->left);
            free_cmd(lcmd->right);

            free(lcmd->right);
            free(lcmd->left);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;

            free_cmd(pcmd->left);
            free_cmd(pcmd->right);

            free(pcmd->right);
            free(pcmd->left);
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;

            free_cmd(bcmd->cmd);

            free(bcmd->cmd);
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;

            free_cmd(scmd->cmd);

            free(scmd->cmd);
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }
}

void run_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;
    int p[2];
    int fd;
    int auxfd;
    int status2;
    pid_t pid;
    pid_t pid_r;

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            //compruebo si es un comando interno
            //devuelve 1 si no es un comando interno y creo un fork
            if(checkCommand(ecmd) == 1){
            	bloqueoSIGCHLD();
                if ((pid = fork_or_panic("fork EXEC")) == 0)
                    exec_cmd(ecmd);
                TRY( waitpid(pid,&status,0) );
                desbloqueoSIGCHLD();
            }
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            auxfd = dup(rcmd->fd);
            TRY (close(rcmd->fd));
            if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }

            run_cmd(rcmd->cmd);
            dup2(auxfd, 1);

            break;
        case LIST:
            lcmd = (struct listcmd*) cmd;
            run_cmd(lcmd->left);
            run_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*)cmd;
            if (pipe(p) < 0)
            {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
            bloqueoSIGCHLD();
            // Ejecución del hijo de la izquierda
            if ((pid = fork_or_panic("fork PIPE left")) == 0)
            {
                TRY( close(1) );
                TRY( dup(p[1]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );

               if (pcmd->left->type == EXEC){
                    ecmd = (struct execcmd*) pcmd->left;
                    //compruebo si es comando interno
                    if(checkCommand(ecmd) == 1)
                        exec_cmd(ecmd);
                }else
                    run_cmd(pcmd->left);
                
                exit(EXIT_SUCCESS);
            }

            // Ejecución del hijo de la derecha
            if ((pid_r = fork_or_panic("fork PIPE right")) == 0)
            {
                TRY( close(0) );
                TRY( dup(p[0]) );
                TRY( close(p[0]) );
                TRY( close(p[1]) );
                if (pcmd->right->type == EXEC){
                    ecmd = (struct execcmd*) pcmd->right;
                    //compruebo si es comando interno
                    if(checkCommand(ecmd) == 1)
                        exec_cmd(ecmd);
                }else
                    run_cmd(pcmd->right);

                exit(EXIT_SUCCESS);
            }
            TRY( close(p[0]) );
            TRY( close(p[1]) );

            // Esperar a ambos hijos
            TRY( waitpid(pid,&status,0) );
            TRY( waitpid(pid_r,&status2,0) );
            desbloqueoSIGCHLD();
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            bloqueoSIGCHLD();
            if ((pid = fork_or_panic("fork BACK")) == 0)
            {
                if (bcmd->cmd->type == EXEC){
                    ecmd = (struct execcmd*) bcmd->cmd;
                    if(checkCommand(ecmd) == 1)
                        exec_cmd(ecmd);
               		}else
                    	run_cmd(bcmd->cmd);

				exit(EXIT_SUCCESS);
            }

            //add a child process 
            //si se supera 8 procesos no puede añadir más
            int i = 0;
  			while(secondProcess[i]!=0 && i<MAX_PROCESS){
    			i++;
 			}
 			if(secondProcess[i]==0) //si esto no se cumple quiere decir que se ha superado 8 child process
  				secondProcess[i] = pid;
        	
        	fprintf(stdout,"[%d]\n",pid);
            desbloqueoSIGCHLD();
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            bloqueoSIGCHLD();   
            if ((pid = fork_or_panic("fork SUBS")) == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( waitpid(pid,&status,0) );
            desbloqueoSIGCHLD();
            break;

        case INV:
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);
    }

    DPRINTF(DBG_TRACE, "END\n");
}

void print_cmd(struct cmd* cmd)
{
    struct execcmd* ecmd;
    struct redrcmd* rcmd;
    struct listcmd* lcmd;
    struct pipecmd* pcmd;
    struct backcmd* bcmd;
    struct subscmd* scmd;

    if(cmd == 0) return;

    switch(cmd->type)
    {
        default:
            panic("%s: estructura `cmd` desconocida\n", __func__);

        case EXEC:
            ecmd = (struct execcmd*) cmd;
            if (ecmd->argv[0] != 0)
                printf("fork( exec( %s ) )", ecmd->argv[0]);
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            printf("fork( ");
            if (rcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) rcmd->cmd)->argv[0]);
            else
                print_cmd(rcmd->cmd);
            printf(" )");
            break;

        case LIST:
            lcmd = (struct listcmd*) cmd;
            print_cmd(lcmd->left);
            printf(" ; ");
            print_cmd(lcmd->right);
            break;

        case PIPE:
            pcmd = (struct pipecmd*) cmd;
            printf("fork( ");
            if (pcmd->left->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->left)->argv[0]);
            else
                print_cmd(pcmd->left);
            printf(" ) => fork( ");
            if (pcmd->right->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) pcmd->right)->argv[0]);
            else
                print_cmd(pcmd->right);
            printf(" )");
            break;

        case BACK:
            bcmd = (struct backcmd*) cmd;
            printf("fork( ");
            if (bcmd->cmd->type == EXEC)
                printf("exec ( %s )", ((struct execcmd*) bcmd->cmd)->argv[0]);
            else
                print_cmd(bcmd->cmd);
            printf(" )");
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            printf("fork( ");
            print_cmd(scmd->cmd);
            printf(" )");
            break;
    }
}


/******************************************************************************
 * Lectura de la línea de órdenes con la biblioteca libreadline
 ******************************************************************************/


// `get_cmd` muestra un *prompt* y lee lo que el usuario escribe usando la
// biblioteca readline. Ésta permite mantener el historial, utilizar las flechas
// para acceder a las órdenes previas del historial, búsquedas de órdenes, etc.

char* get_cmd()
{
    char* buf;

    //obtengo uid del usuario actual
    uid_t uid = getuid();
    //obtengo la estructura passwd 
    struct passwd* pwd = getpwuid(uid);
    if(!pwd){
    	perror("getpwuid");
    	exit(EXIT_FAILURE);
    }
    //obtengo el username del usuario actual; ej)alumno
    char *username = pwd->pw_name;
    //obtengo la ruta absoluta
    char *ruta = get_path();
    //obtengo la ruta relativa
    char *relativa= basename(ruta);

    char* prompt = malloc(strlen(username)+strlen(relativa)+4);
    sprintf(prompt, "%s@%s> ", username, relativa);
    free(ruta);

    buf = readline(prompt);
    free(prompt);
    // Si el usuario ha escrito una orden, almacenarla en la historia.
    if(buf)
        add_history(buf);

    return buf;
}

void freeMemory(){
    free_cmd(cmd);
    free(cmd);
}


/******************************************************************************
 * Bucle principal de `simplesh`
 ******************************************************************************/


void help(int argc, char **argv)
{
    info("Usage: %s [-d N] [-h]\n\
         shell simplesh v%s\n\
         Options: \n\
         -d set debug level to N\n\
         -h help\n\n",
         argv[0], VERSION);
}


void parse_args(int argc, char** argv)
{
    int option;

    // Bucle de procesamiento de parámetros
    while((option = getopt(argc, argv, "d:h")) != -1) {
        switch(option) {
            case 'd':
                g_dbg_level = atoi(optarg);
                break;
            case 'h':
            default:
                help(argc, argv);
                exit(EXIT_SUCCESS);
                break;
        }
    }
}

//handle_sigchld define el manejador para la señal SIGCHLD
void handle_sigchld(int sig) {
	int saved_errno = errno;
	pid_t pid;
	int writ = 0;

	char buffer[16];
	memset(buffer,'\0',16);

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
  		//escribo [PID] ya que ese proceso habrá terminado
  		sprintf(buffer,"[%u]\n",pid);
        writ = write(STDOUT_FILENO,buffer,strlen(buffer));
        if(writ < 0){
        	perror("handle_sigchld -> write");
        	exit(EXIT_FAILURE);
        }
    	//elimino el proceso de la lista de procesos en seg. plano
    	int i=0;
        for (int i = 0; i < MAX_PROCESS; i++){
            if (secondProcess[i]==pid){
                secondProcess[i]=0;
            }
        }
    }
    errno = saved_errno;
}

// `bloqueoSIGCHLD` se usa para bloquear el handler_sigchld de manera
// que no interrumpa los procesos en primer plano
void bloqueoSIGCHLD()
{
    int error = sigprocmask(SIG_BLOCK, &signalChld, NULL);
	if (error == -1){
	    perror("run_cmd -> sigprocmask: block");
	    exit(EXIT_FAILURE);
    }
}

//`desbloqueoSIGCHLD` se usa para desbloquear el handler_sigchld 
void desbloqueoSIGCHLD()
{
	int error = sigprocmask(SIG_UNBLOCK, &signalChld, NULL);
	if (error == -1){
	   perror("run_cmd -> sigprocmask: unblock");
	   exit(EXIT_FAILURE);
    }
}

int main(int argc, char** argv){
    char* buf;
    //struct cmd* cmd;

    //inicializo a 0 la lista de procesos en segundo plano
    memset(secondProcess,0,sizeof(secondProcess));

    sigemptyset(&signalChld);
	sigaddset(&signalChld, SIGCHLD);

    //bloqueo de la señal SIGINT enviada por CTRL+C
    sigset_t signalInit;
    sigemptyset(&signalInit);
    sigaddset(&signalInit, SIGINT);
	if (sigprocmask(SIG_BLOCK, &signalInit, NULL) == -1){
		perror("MAIN -> SIGPROCMASK: SIGINIT");
		exit(EXIT_FAILURE);
	}

	struct  sigaction  sa;
	memset (&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN; //para ignorar la señal SIGQUIT
	sigemptyset (&sa.sa_mask);
	if (sigaction(SIGQUIT , &sa, NULL) ==  -1) {
		perror("sigaction -> SIGQUIT");
		exit(EXIT_FAILURE);
	}
	//evitar procesos zombies con sigchld
	sa.sa_handler = handle_sigchld;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL) == -1){
		perror("MAIN -> SIGACTION");
		exit(EXIT_FAILURE);
	}
	
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");

    if (unsetenv("OLDPWD") == -1) {
        perror("MAIN -> UNSETENV");
        exit(EXIT_FAILURE);
    }

    // Bucle de lectura y ejecución de órdenes
    while ((buf = get_cmd()) != NULL){
        // Realiza el análisis sintáctico de la línea de órdenes
        cmd = parse_cmd(buf);

        // Termina en `NULL` todas las cadenas de las estructuras `cmd`
        null_terminate(cmd);

        DBLOCK(DBG_CMD, {
            info("%s:%d:%s: print_cmd: ",
                 __FILE__, __LINE__, __func__);
            print_cmd(cmd); printf("\n"); fflush(NULL); } );

        // Ejecuta la línea de órdenes
        run_cmd(cmd);

        // Libera la memoria de las estructuras `cmd` y cmd
        freeMemory();

        // Libera la memoria de la línea de órdenes
        free(buf);
    }

    DPRINTF(DBG_TRACE, "END\n");

    return 0;
}
