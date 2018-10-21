/*
 * Shell `simplesh` (basado en el shell de xv6)
 *
 * Ampliación de Sistemas Operativos
 * Departamento de Ingeniería y Tecnología de Computadores
 * Facultad de Informática de la Universidad de Murcia
 *
 * Alumnos: APELLIDOS, NOMBRE (GX.X)
 *          APELLIDOS, NOMBRE (GX.X)
 *
 * Convocatoria: FEBRERO/JUNIO/JULIO
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

struct cmd* cmd;
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

void print_help_hd(){
//Impresión de la ayuda del comando, se ha introducido correctamente el: hd -h
fprintf(stdout, "Uso: hd [-l NLINES] [-b NBYTES] [-t BSIZE] [FILE1] [FILE2] ...\n");
fprintf(stdout, "\tOpciones:\n");
fprintf(stdout, "\t-l NLINES Número máximo de líneas a mostrar.\n");
fprintf(stdout, "\t-b NBYTES Número máximo de bytes a mostrar.\n");
fprintf(stdout, "\t-t BSIZE Tamaño en bytes de los bloques leídos de [FILEn] o stdin.\n");
fprintf(stdout, "\t-h help\n");
}

void parse_data(int file_descriptor, int tsize, int lsize, int bsize){
   int n_lineas = 0;
   int n_bytes = 0;
   char buffer[tsize];
   char buffer_aux[tsize];
   int readed, aux_size, index_b;
   memset(buffer, 0, tsize);
   memset(buffer_aux, 0, tsize);	
	while((readed = read(file_descriptor, buffer, tsize))>0){			
	//Tengo que controlar que no sea la última línea
	if (n_lineas < lsize && lsize != -1){
	  index_b = 0;
	  while(n_lineas < lsize && index_b < readed){
		buffer_aux[index_b] = buffer[index_b];
		if (buffer[index_b] == '\n' )
		  n_lineas ++;
		index_b ++;
	  }
	  write(STDOUT_FILENO, buffer_aux, index_b);
	}
	if(bsize != -1){
	if((n_bytes + readed)<= bsize){
	     write(STDOUT_FILENO, buffer, readed);
	     n_bytes += readed;
	  }else{
	    aux_size = 0;
	    if(n_bytes == bsize) break;
	     for(int o = n_bytes; o < bsize; o++){
		aux_size ++;						
	     }
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
	    int readed, n_lineas, n_bytes;
	    n_lineas = 0;
	    n_bytes = 0;
			
	//Caso de solicitud de parámetro -h
	if(cmd->argc > 1){

	if (strcmp(cmd->argv[1], "-h") == 0){	
		//Si hay más de dos argumentos no se ha construido bien el comando. Debe ser: hd -h		
		if(cmd->argc > 2){
			fprintf(stderr, "hd: Opción no válida\n");
			return;
		}   
		print_help_hd();
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
			   break;		
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
			   break;		
			}
			break;
		    default:
			fprintf(stderr, "Usage: %s [-f] [-n NUM]\n", cmd->argv[0]);
		}
	    }
	if(lsize == -1 && bsize == -1) lsize = 3;
	if(optind == cmd->argc){    
	   parse_data(STDIN_FILENO, tsize, lsize, bsize); 
	}else{
		//Recorre los ficheros del comando
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
//Imprime el texto de ayuda del comando src
void print_help_src(){
fprintf(stdout, "Uso: src [-d DELIM] [FILE1] [FILE2]...\n");
fprintf(stdout, "\tOpciones:\n");
fprintf(stdout, "\t-d DELIM Carácter de inicio de comentarios.\n");
fprintf(stdout, "\t-h help\n");
}

					
//Comprueba que el caracter introducido en la opción -d sea válido (Caracteres especiales tipo: % $) 
int test_delimiter(char * delim){
//Cualquier carácter que no sea una letra o digito numérico
regex_t r;
char * regex = "^[^a-zA-Z0-9&;|]{1}$";
int exec ;
//Compilación de la expresión regular
int status = regcomp (&r, regex, REG_EXTENDED|REG_NEWLINE);
    if (status == 0) {
//Comprobación de cumplimento de la ER
	exec = regexec(&r, delim, 0, NULL, 0);
//Libera la memoria reservada
	regfree(&r);
	return exec;
    }else{
	return 1;

   }
}

void parse_source(int file_descriptor, int tsize, char delim){
   int n_lineas = 0;
   char buffer[tsize];
   char * token, ptrInicio, ptrFinal;
   int readed, aux_lineas, index_b, index_lastline;
   memset(buffer, 0, tsize);
   struct cmd* cmd1;
  // memset(buffer_aux, 0, tsize);
	while((readed = read(file_descriptor, buffer, tsize))>0){
		token = strtok(buffer, "\n");
		while(token!= NULL){
		   ptrInicio = *token;	
		   ptrFinal = *(token+strlen(token)-1);		   
		   if(ptrInicio != delim && ptrFinal != delim ){
			cmd1 =  parse_cmd(token);
			null_terminate(cmd1);
			run_cmd(cmd1);
			free_cmd(cmd1);
    			free(cmd1);
			
		   }
		   token = strtok(NULL, "\n");

		}	
	memset(buffer, 0, tsize);			   	
   	}	  
}
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
			if ((cmd->argc - optind) == 0){
			     print_help_src();
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
		//Recorre los ficheros disponibles para ejecutar las órdenes
		for(int i = optind; i < cmd->argc; i++){
			if((file_descriptor = open(cmd->argv[i], O_RDONLY)) != -1){
				parse_source(file_descriptor, tsize, delim);			

			}else{
			   fprintf(stdout, "src: No se encontró el archivo '%s'\n", cmd->argv[i]);
			}
			
		}

	}
}

void freeMemory();

void run_exit(){
    freeMemory();
    exit(EXIT_SUCCESS);
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

    DPRINTF(DBG_TRACE, "STR\n");

    if(cmd == 0) return;

    switch(cmd->type)
    {
        case EXEC:
            ecmd = (struct execcmd*) cmd;
            //compruebo si es un comando interno
            //devuelve 1 si no es un comando interno y creo un fork
            if(checkCommand(ecmd) == 1){
                if ((fork_or_panic("fork EXEC")) == 0)
                    exec_cmd(ecmd);
                TRY( wait(NULL) );
            }
            break;

        case REDR:
            rcmd = (struct redrcmd*) cmd;
            /*if (fork_or_panic("fork REDR") == 0)
            {
                TRY( close(rcmd->fd) );
                if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0)
                {
                    perror("open");
                    exit(EXIT_FAILURE);
                }

                if (rcmd->cmd->type == EXEC)
                    exec_cmd((struct execcmd*) rcmd->cmd);
                else
                    run_cmd(rcmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( wait(NULL) );
            break;*/
            auxfd = dup(rcmd->fd); //llamada al sistema que duplica el descr de fichero
            TRY (close(rcmd->fd));
            if ((fd = open(rcmd->file, rcmd->flags, rcmd->mode)) < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }
            run_cmd(rcmd->cmd);
            dup2(auxfd, 1); //para que apunte el fd stdout al desc de fichero de la redirección
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

            // Ejecución del hijo de la izquierda
            if (fork_or_panic("fork PIPE left") == 0)
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
            if (fork_or_panic("fork PIPE right") == 0)
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
            TRY( wait(NULL) );
            TRY( wait(NULL) );
            break;

        case BACK:
            bcmd = (struct backcmd*)cmd;
            if (fork_or_panic("fork BACK") == 0)
            {
                if (bcmd->cmd->type == EXEC){
                    ecmd = (struct execcmd*) bcmd->cmd;
                    if(checkCommand(ecmd) == 1)
                        exec_cmd(ecmd);
                }else
                    run_cmd(bcmd->cmd);

                exit(EXIT_SUCCESS);
            }
            break;

        case SUBS:
            scmd = (struct subscmd*) cmd;
            if (fork_or_panic("fork SUBS") == 0)
            {
                run_cmd(scmd->cmd);
                exit(EXIT_SUCCESS);
            }
            TRY( wait(NULL) );
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


int main(int argc, char** argv){
    char* buf;
    //struct cmd* cmd;
    
    unsetenv("OLDPWD");
    
    parse_args(argc, argv);

    DPRINTF(DBG_TRACE, "STR\n");

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