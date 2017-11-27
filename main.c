#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include "prototype.h"
#include "history.h"
#include "alias.h"


int inarg(char c);

int runcommand(int argc, char **cline, char where);

static char inpbuf[MAXBUF], tokbuf[3 * MAXBUF], *ptr, *tok;
const char special[] = { ' ', '\t', '&', ';', '\n', '\0', '|', '<', '>'};
const char *command[] = {"cd", "jobs", "exit", "fg", "history", "set", "unset", "export", "help", "echo", "!\0", "alias", "unalias"};
char *shellval[100] = {};

struct termios oldTer;
struct sigaction act_stp;
struct sigaction act_int;
struct sigaction act_old;

pthread_t thread_t;
int sig_fd[2];
pid_t sg_pid;
int thread_status;
int thread_exit_switch = 0;

stCurrentProc c_proc;

pMybgList head;
pMySetList setHead;


void help() {
    printf("***************************************MiniShell*************************************\n");
    printf("*      This shell is a mini shell made up of UNIX SYSTEM PROGRAMING SUBJECT.        *\n");
    printf("* [*]Built-in Commands.                                                             *\n");
    printf("*   - cd, jobs, fg, history, set, unset, export, help, echo, exit, (un)alias.       *\n");
    printf("* [*]Supported operations.                                                          *\n");
    printf("*   - Redirection,  Pipe.                                                           *\n");
    printf("*************************************************************************************\n");
}

void initTC() {
    struct termios new;
    tcgetattr(0, &oldTer);
    memcpy(&new, &oldTer, sizeof(struct termios));

    new.c_lflag &= ~(ICANON | ECHO);
    new.c_cc[VMIN] = 1;
    new.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &new);
    return;
}


void resetTC() {
    tcsetattr(0, TCSANOW, &oldTer);
    return;
}



int userin(char *p) {
    int c, count;
    struct _alias *ali;
    memset(inpbuf, 0, MAXBUF);
    ptr = inpbuf;
    char* commandBuf;
    tok = tokbuf;
    /* display prompt */
    printf("[%s]%s ", getenv("PWD"), p);
    count = 0;
    while ( (c = fgetc(stdin)) ) {
        if (c == 10 && count < MAXBUF - 1) {
            if(count == 0){
                printf("\n");
                return 0;
            }
            if (inpbuf[0] != '!')
                addHistory(inpbuf);
            ali = getAlias(inpbuf);
            if (ali != NULL) {
                count = strlen(ali->value);
                strncpy(inpbuf, ali->value, count);
            }
            inpbuf[count++] = '\n';
            inpbuf[count] = '\0';
            printf("\n");
            return 1;
        }
        if( (c <= 26) && (c != 8)) continue;
        switch(c) {
            case 127:
                if (count == 0) continue;
                fputc('\b', stdout);
                fputc(' ', stdout);
                fputc('\b', stdout);
                inpbuf[--count] = (char) 0;
                continue;
            case 27:
                if ((c = fgetc(stdin)) != 91)
                    continue;

                switch (fgetc(stdin)) {
                    case 65:
                        commandBuf = popPrev();
                        if (commandBuf != NULL) {
                            fprintf(stdout, "\r%10s", " ");
                            fprintf(stdout, "\r[%s]%s %s", getenv("PWD"), p, commandBuf);
                            strncpy(inpbuf, commandBuf, strlen(commandBuf));
                            count = (int) strlen(commandBuf);
                        }
                        continue;
                    case 66:
                        //down
                        commandBuf = popNext();
                        if (commandBuf != NULL) {
                            fprintf(stdout, "\r%80s", " ");
                            fprintf(stdout, "\r[%s]%s %s", getenv("PWD"), p, commandBuf);
                            strncpy(inpbuf, commandBuf, strlen(commandBuf));
                            count = (int) strlen(commandBuf);
                        }
                        continue;
                    case 67:
                        continue;
                    case 68:
                        continue;
                }
                break;
            default:
                fputc(c, stdout);
                inpbuf[count++] = (char) c;
                continue;
        }
        /* if line too long, restart */
        if(strlen(inpbuf) == 0) return 0;
        return 1;
    }
    return 0;
}



void changeValue(char** cline, int narg){
    int i = 0;
    char* val;
    while(cline[i] && i <= narg){
        if (cline[i][0] == '$') {
            pMySetList t = findSetNodeByName(&cline[i][1], setHead);
            if (t != NULL) {
                cline[i] = t->value;
            } else {
                val = getenv(&cline[i][1]);
                if (!val) {
                    cline[i] = "\t";
                } else {
                    cline[i] = val;
                }
            }
        }
        i++;
    }
    return;
}

int gettok(char **outptr) {
    int type;

    *outptr = tok;

    while (*ptr == ' ' || *ptr == '\t') ptr++;

    *tok++ = *ptr;

    switch (*ptr++) {
        case '\n' :
            type = EOL;
            break;
        case '&' :
            type = AMPERSAND;
            break;
        case ';' :
            type = SEMICOLON;
            break;
        case '|' :
            type = PIPE;
            break;
        case '<' :
            type = REDIRECTION_LEFT;
            break;
        case '>' :
            if (*ptr == '>') {
                type = REDIRECTION_ADD;
                *ptr++;
            } else {
                type = REDIRECTION_RIGHT;
            }
            break;
        default :
            type = ARG;

            while (inarg(*ptr)) *tok++ = *ptr++;
            break;
    }
    *tok++ = '\0';
    return type;
}

/* are we in an ordinary argument */
int inarg(char c) {
    char *wrk;
    for (wrk = (char *) special; *wrk != '\0'; wrk++) {
        if (c == *wrk) {
            //printf(" special arg : %c inarg()\n", *wrk);
            return 0;
        }
    }
    return 1;
}


void procline() {
    char *arg[MAXARG + 1];
    int toktype;
    int narg;
    int type = 0;
    int PipeFlag;

    for (narg = 0;;) {
        toktype = gettok(&arg[narg]);
        changeValue(arg, narg);
        switch (toktype) {
            case ARG :
                if (narg < MAXARG) narg++;
                break;
            case EOL :
                type = 0;
                if (narg != 0) {
                    arg[narg] = NULL;
                    runcommand(narg, arg, type);
                }
                return;
            case SEMICOLON :
                type = 0;
                if (narg != 0) {
                    arg[narg] = NULL;
                    runcommand(narg, arg, type);
                }
                narg = 0;
                break;

            case AMPERSAND :
                type = (toktype == AMPERSAND) ?
                       BACKGROUND : FOREGROUND;
                if (narg != 0) {
                    arg[narg] = NULL;
                    runcommand(narg, arg, type);
                }
                if (toktype == EOL) return;
                narg = 0;
                break;
            case PIPE :
                if (narg != 0) {
                    arg[narg] = NULL;
                    arg[narg++] = NULL;
                    PipeFlag = narg;
                    while (gettok(&arg[narg]) != EOL) {
                        narg++;
                    }
                    arg[narg] = NULL;
                    pipelining(arg, arg + PipeFlag);
                }
                narg = 0;
                return;
            case REDIRECTION_LEFT :
                reAndPipe(REDIRECTION_LEFT, arg, narg, type);
                narg = 0;

                break;
            case REDIRECTION_RIGHT :
                reAndPipe(REDIRECTION_RIGHT, arg, narg, type);
                narg = 0;

                break;
            case REDIRECTION_ADD :
                reAndPipe(REDIRECTION_ADD, arg, narg, type);
                narg = 0;
                break;
        }
    }
}

void reAndPipe(int direcType, char **arg, int narg, int type) {
    int oldfd = -1;

    if (narg != 0) {
        gettok(&arg[narg]);
        narg++;
        arg[narg] = NULL;
        oldfd = redirection(direcType, arg[--narg]);
        arg[narg] = NULL;
        runcommand(narg, arg, type);

        if (direcType == REDIRECTION_LEFT)
            dup2(oldfd, 0);
        else
            dup2(oldfd, 1);
        close(oldfd);
        oldfd = -1;
    }
}


void exitsh() {
    thread_exit_switch = 1;
    write(sig_fd[1], "d", 1);
    pthread_join(thread_t, (void **) &thread_status);
    deleteAllBgNode(head);
    deleteAllSetNode(setHead);
    destroyHistory();
    destroyAlias();
    resetTC();
    printf("Minish exit.\n");
    exit(0);
}

void commandDispatcher(int cmd, char **cline, char argc) {

    int APPLE = 0;
    char *command, *key, *value;
    switch (cmd) {
        case MINICMD_CD:
            if (cd(cline[1]) == -1) {
                perror("CD");
            }
            break;
        case MINICMD_EXIT:
            exitsh();
        case MINICMD_EXPORT:
            export(cline);
            break;
        case MINICMD_HELP:
            help();
            break;
        case MINICMD_HISTORY:
            printHistory();
            break;
        case MINICMD_JOBS:
            jobs();
            break;
        case MINICMD_SET:
            set(cline);
            break;
        case MINICMD_UNSET:
            unset(cline);
            break;
        case MINICMD_FG:
            if (cline[1][0] != '%') {
                break;
            }
            if (fg(atoi(&cline[1][1])) == -1) {

                return;
            }
            break;
        case MINICMD_ECHO:
            echo(cline);
            break;
        case MINICMD_HIS:
            command = cline[0] + 1;
            APPLE = atoi(command);
            command = getHistory(APPLE);
            if (command == NULL) printf("Invalid number");
            else {
                ptr = inpbuf;
                tok = tokbuf;
                strcpy(inpbuf, command);
                procline();
            }
            break;

        case MINICMD_ALIAS:
            if (cline[1] != NULL && strstr(cline[1], "=") != NULL) {
                key = strtok(cline[1], "=");
                value = strtok(NULL, "=");
                setAlias(key, value);
                printf("success.\n");
            }
            else {
                printAlias();
            }
            break;

        case MINICMD_UNALIAS:
            if (strlen(cline[1]) < 1) return;

            if (!strcmp(cline[1], "-a"))
                unAlias(NULL);
            else
                unAlias(cline[1]);

            printf("success.\n");
            break;
        default:
            return;
    }
    return;
}


int mh_commandHandler(char argc, char **cline) {
    for (int i = 0; i < COMMANDNUM; i++) {
        if (!strcmp(cline[0], command[i])) {
            commandDispatcher(i, cline, argc);
            return 0;
        }
    }

    if (cline[0][0] == '!') {
        commandDispatcher(MINICMD_HIS, cline, argc);
        return 0;
    }
    return -1;
}


int runcommand(int argc, char **cline, char where) {
    pid_t pid; // , exitstat, ret;
    int status;

    if (!mh_commandHandler(argc, cline)) {
        return 0;
    }

    if ((pid = fork()) < 0) {
        perror("Minish");
        return -1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        execvp(*cline, cline);
        perror(*cline);
        exit(127);
    }

    if (where == BACKGROUND) {
        printf("[Process id %d\t", pid);
        pMybgList t = addList(pid, *cline, head, BGLIST_STATUS_START);
        printf("jid : %d]\n", getJid(t, head));
        return 0;
    }
    c_proc.st_pid = pid;
    strcpy(c_proc.fgName, *cline);
    if (waitpid(pid, &status, WUNTRACED) == -1) {
        c_proc.fgName[0] = (char) NULL;
        c_proc.st_pid = 0;
        return -1;
    } else {
        c_proc.fgName[0] = (char) NULL;
        c_proc.st_pid = 0;
        return status;
    }
}


int main(void) {

    if (initHistory(10, 512, 100) < 0) {
        printf("history Init failed!\n");
        return 0;
    }
    initTC();
    initAlias(50);

    act_int.sa_handler = signalHandler;
    act_stp.sa_handler = signalHandler;

    sigemptyset(&act_stp.sa_mask);
    sigemptyset(&act_int.sa_mask);
    signal(SIGKILL, SIG_IGN);
    sigaction(SIGTSTP, &act_stp, &act_old);
    sigaction(SIGINT, &act_int, &act_old);
    signal(SIGCHLD, signalHandler_cld);
    pipe(sig_fd);

    if (pthread_create(&thread_t, NULL, sigProc, NULL) < 0) {
        perror("thread create");
        exit(0);
    }

    setpgid(0, 0);
    head = initializeList();
    setHead = initializeSetList();
    help();
    while (1) {
        if(userin(SHPROMPT) == 1) {
            procline();
        }
    }
}

