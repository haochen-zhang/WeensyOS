#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <iostream>
#include <map>
#include <utility>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__


// signal interrupt handler
//    catch interrupt and change interrupt to 1
volatile sig_atomic_t interrupt = 0;

void int_handler(int i) {
    (void) i;
    interrupt = 1;
}


// struct command
//    Data structure describing a command. Add your own stuff.

struct command {
    std::vector<std::string> args;
    pid_t pid = -1;          // process ID running this command, -1 if none
    pid_t pgid = -1;         // process pgid, -1 if none
    int op;                  // operator flag
    int status;              // the most updated status code
    int read_fd;             // pipe read end number
    int write_fd;            // pipe write end number
    std::vector <std::pair <std::string, std::string>> redirect_list;

    command* prev;           // command chain previous pointer
    command* next;           // command chain next pointer
    
    command();
    ~command();

    pid_t run();
    bool left_hand_pipe();
    bool right_hand_pipe();
    bool chain_in_background();
    bool run_conditional();
    bool right_hand_redirect();
};


// command::command()
//    This constructor function initializes a `command` structure. You may
//    add stuff to it as you grow the command structure.

command::command() {
    this->op = TYPE_SEQUENCE;
    this->prev = nullptr;
    this->next = nullptr;
    this->status = 0;
    this->read_fd = -1;
    this->write_fd = -1;
    this->redirect_list = {};
}


// command::~command()
//    This destructor function is called to delete a command.

command::~command() {
}


// command::left_hand_pipe()
//    This boolean function is called to check if a command is at the left of a pipe

bool command::left_hand_pipe() {
    return this->op == TYPE_PIPE;
}


// command::left_hand_pipe()
//    This boolean function is called to check if a command is at the right of a pipe

bool command::right_hand_pipe() {
    return this->prev != nullptr && this->prev->op == TYPE_PIPE;
}


// command::left_hand_pipe()
//    This boolean function is called to check if a command is at the right of a pipe

bool command::right_hand_redirect() {
    return this->prev != nullptr && this->prev->op == TYPE_REDIRECT_OP;
}


// command::chain_in_background()
//    This boolean function is called to check if a command is at the right of a pipe

bool command::chain_in_background() {
    command* c = this;
    if (c->op != TYPE_SEQUENCE && c->op != TYPE_BACKGROUND) {
        c = c->next;
    }
    return c->op == TYPE_BACKGROUND;
}


// run_conditional(c)
//     handle conditional command chained by && and ||
//     condition 1: linked by && and previous exit status == 0: return True;
//     condition 2: linked by && and previous exit status == 1: return False, get previous status code 1;
//     condition 3: linked by || and previous exit status == 0: return False, get previous status code 0;
//     condition 4: linked by || and previous exit status == 1: return True;

bool command::run_conditional() {
    if (this->prev && this->prev->op == TYPE_AND) {
        if (WIFEXITED(this->prev->status) && WEXITSTATUS(this->prev->status) == 0) {
            this->status = 0;
            return true;
        } else {
            this->status = 1;
            return false;
        }
    } else if (this->prev && this->prev->op == TYPE_OR) {
        if (WIFEXITED(this->prev->status) && WEXITSTATUS(this->prev->status) == 0) {
            this->status = 0;
            return false;
        } else {
            this->status = 0;
            return true;
        }
    } else if (this->right_hand_pipe()) {
        if (this->prev->status == 1) {
            this->status = 1;
            return false;
        }
    }
    return true;
}


// COMMAND EXECUTION

// command::run()
//    Create a single child process running the command in `this`.
//    Sets `this->pid` to the pid of the child process and returns `this->pid`.
//
//    PART 1: Fork a child process and run the command using `execvp`.
//       This will require creating an array of `char*` arguments using
//       `this->args[N].c_str()`.
//    PART 5: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PART 7: Handle redirections.
//    PART 8: Update the `command` structure and this function to support
//       setting the child process’s process group. To avoid race conditions,
//       this will require TWO calls to `setpgid`.

pid_t command::run() {
    // Chang directory check
    //      call chdir to change the directory
    //      update command status according to chdir return value
    //      return pid directly without forking
    if (this->args[0] == "cd") {
        int st = chdir(this->args[1].c_str());
        if (st == -1) {
            this->status = 1;
            return this->pid;
        }
        this->status = 0;
        return this->pid; 
    }

    // Fork a new child process
    //      check child process status, execute command
    //      check parent process status, return child process pid and close all pipes
    pid_t c_pid = fork();
    if (c_pid == -1) {              // fail to fork
        fprintf(stderr, "command::run fork() failed to execute\n");
        this->status = 1;
        _exit(1);
    } else if (c_pid == 0) {        // child process
        setpgid(0, this->pgid);     // set child process pgid

        // Pipe reading or writing
        //      pipe already assigned in run_list
        //      here implement dup2 and close corresponding pipe
        if (this->left_hand_pipe()) {
            dup2(this->write_fd, STDOUT_FILENO);
            close(this->write_fd);
        }
        if (this->right_hand_pipe()) {
            dup2(this->read_fd, STDIN_FILENO);
            close(this->read_fd);
        }

        // Redrection
        //      redirection informations are stored in redirect_list vector(type, filename)
        //      handle different redirection accordingly
        for (auto it: this->redirect_list) {
            if (strcmp(it.first.c_str(), ">") == 0) {
                int fd_out = open(it.second.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
                if (fd_out < 0) {
                    fprintf(stderr, "No such file or directory\n");
                    _exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            } else if (strcmp(it.first.c_str(), "<") == 0) {
                int fd_in = open(it.second.c_str(),O_RDONLY);
                if (fd_in < 0) {
                    fprintf(stderr, "No such file or directory\n");
                    _exit(1);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            } else {
                int fd_err = open(it.second.c_str(),O_WRONLY|O_CREAT|O_TRUNC, 0666);
                if (fd_err < 0) {
                    fprintf(stderr, "No such file or directory\n");
                    _exit(1);
                }
                dup2(fd_err, STDERR_FILENO);
                close(fd_err);
            }
        }

        // Execvp
        //      store the command
        //      try execvp and catch status code
        std::vector<const char *> argv;
        for (auto it = this->args.begin(); it !=  this->args.end(); ++it) {
            argv.push_back(it->c_str());
        }
        argv.push_back(nullptr);
        int st = execvp(argv[0], (char *const *) argv.data());
        if (st == -1) {
            fprintf(stderr, "command::run execvp() failed to execute\n");
            this->status = 1;
            _exit(1);
        }
    } else {  // parent process
        setpgid(c_pid, this->pgid);
        this->pid = c_pid;
        // Close all the current pipe to correctly start next one
        close(this->write_fd);
        close(this->read_fd);
    }
    return this->pid;
}


// run_list(c)
//    Run the command *list* starting at `c`. Initially this just calls
//    `c->run()` and `waitpid`; you’ll extend it to handle command lists,
//    conditionals, and pipelines.
//
//    It is possible, and not too ugly, to handle lists, conditionals,
//    *and* pipelines entirely within `run_list`, but many students choose
//    to introduce `run_conditional` and `run_pipeline` functions that
//    are called by `run_list`. It’s up to you.
//
//    PART 1: Start the single command `c` with `c->run()`,
//        and wait for it to finish using `waitpid`.
//    The remaining parts may require that you change `struct command`
//    (e.g., to track whether a command is in the background)
//    and write code in `command::run` (or in helper functions).
//    PART 2: Treat background commands differently.
//    PART 3: Introduce a loop to run all commands in the list.
//    PART 4: Change the loop to handle conditionals.
//    PART 5: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PART 8: - Make sure every process in the pipeline has the same
//         process group, `pgid`.
//       - Call `claim_foreground(pgid)` before waiting for the pipeline.
//       - Call `claim_foreground(0)` once the pipeline is complete.


void run_list(command* c) {
    int wait_flag;  // wait flag to store different wait condition value

    while (c) {
        wait_flag = 0;
        if (c->chain_in_background()) {    // handle background commands
            // catch background commands linkedlist
            command* back = c;
            command* ccur = c;
            while (ccur && ccur->op != TYPE_BACKGROUND) {
                ccur = ccur->next;
            }
            c = ccur->next;     // assign start command not in current background list 

            // Fork child process for background commands
            pid_t pid = fork();
            if (pid == 0) {     // child process
                while (back && back->run_conditional() && back != c) {  // check condition
                    wait_flag = 0;
                    if (back->left_hand_pipe()) {   // check pipe
                        int pfd[2];
                        int st = pipe(pfd);
                        if (st < 0) {
                            _exit(1);
                        }
                        back->write_fd = pfd[1];
                        back->next->read_fd = pfd[0];
                        wait_flag = WNOHANG;
                    }
                    back->run();
                    waitpid(pid, &back->status, wait_flag);
                    back = back->next;
                }
                _exit(0);
            } else {      // parent process
                waitpid(back->pid, &back->status, WNOHANG);
            }
        } else {    // handle non-background process
            if (c->run_conditional()){      // check condition
                if (interrupt == 1) {
                    interrupt = 0;
                    c->status = 1;
                    c = c->next;
                    continue;
                }
                if (c->left_hand_pipe()) {  // check pipe
                    int pfd[2];
                    int st = pipe(pfd);
                    if (st < 0) {
                        _exit(1);
                    }
                    c->write_fd = pfd[1];
                    c->next->read_fd = pfd[0];
                    wait_flag = WNOHANG;
                }
                claim_foreground(c->pgid);
                c->run(); 
                claim_foreground(0);
            }
            waitpid(c->pid, &c->status, wait_flag);
            c = c->next;
        }
    }
}


// parse_line(s)
//    Parse the command list in `s` and return it. Returns `nullptr` if
//    `s` is empty (only spaces). You’ll extend it to handle more token
//    types.

command* parse_line(const char* s) {
    shell_parser parser(s);

    // initialize a command line double linked list
    command* chead = nullptr;       // first command in the list
    command* clast = nullptr;       // last command in the list
    command* ccur = nullptr;        // current command being built   

    // Build the command
    // The handout code treats every token as a normal command word.
    // You'll add code to handle operators.
    for (shell_token_iterator it = parser.begin(); it != parser.end(); ++it) {
        if (it.type() == TYPE_NORMAL) {
            if (!ccur) {
                ccur = new command;
                if (clast) {
                    clast->next = ccur;
                    ccur->prev = clast;
                } else {
                    chead = ccur;
                }
            }
            ccur->args.push_back(it.str());
        } else {
            if (it.type() == TYPE_REDIRECT_OP) {    // catch redirecting information
                std::string tp = it.str();
                ++it;
                ccur->redirect_list.emplace_back(tp, it.str());
                continue;
            }
            clast = ccur;
            clast->op = it.type();
            ccur = nullptr;
        }
    }
    return chead;
}


int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);
    set_signal_handler(SIGINT, int_handler);    // handle interruption

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (command* c = parse_line(buf)) {
                run_list(c);
                delete c;
            }
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes and/or interrupt requests
        int status;
        while(waitpid(-1, &status, WNOHANG) > 0) {
            continue;
        }
    }
    return 0;
}