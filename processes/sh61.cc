#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__

// struct command
struct command
{
    std::vector<std::string> args;
    pid_t pid = -1; // process ID running this command, -1 if none
    command *next = nullptr;
    command *prev = nullptr; // setting up linked list
    int link = TYPE_SEQUENCE;
    int state;
    bool executable = true; // is command executable based on operator logic
    // pipe fd array
    int pipefd[2] = {-1, -1};
    std::string file_i;
    std::string file_o;
    std::string error_f;
    command();
    ~command();
    void run();
};

// command::command()
//    This constructor function initializes a `command` structure.

command::command()
{
}

// command::~command()
//    This destructor function is called to delete a command.

command::~command()
{
    delete next;
}

// COMMAND EXECUTION

// command::run()
// helper function that takes care of redirection
void redirect(const std::string &file, int fd, int flags)
{
    if (!file.empty())
    {
        int file_fd = open(file.c_str(), flags, 0666);
        if (file_fd < 0)
        {
            perror("open");
            _exit(1);
        }
        dup2(file_fd, fd);
        close(file_fd);
    }
}

void command::run()
{
    assert(this->pid == -1);
    assert(this->args.size() > 0);

    pid = fork(); // child pid
    if (pid < 0)
    { // Error
        perror("fork");
        _exit(EXIT_FAILURE);
    }
    else if (pid == 0)
    { // Child process

        // First command
        if (pipefd[0] != -1)
        {
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
        }
        if (pipefd[1] != -1)
        {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
        }
        // using helper function
        redirect(file_i, STDIN_FILENO, O_RDONLY);
        redirect(file_o, STDOUT_FILENO, O_WRONLY | O_CREAT | O_TRUNC);
        redirect(error_f, STDERR_FILENO, O_WRONLY | O_CREAT | O_TRUNC);

        // convert to char* since that is the format that is expected by execvp
        std::vector<char *> argv;
        for (auto &arg : this->args)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        perror("execvp");
        _exit(1);
    }
    else if (pid > 0)
    { // Parent process
        if (pipefd[1] != -1)
        {
            close(pipefd[1]);
        }
        if (pipefd[0] != -1)
        {
            close(pipefd[0]);
        }
    }
    // cd
    if (strcmp(this->args[0].c_str(), "cd") == 0)
    {
        this->state = abs(chdir(this->args[1].c_str()));
        return;
    }
}

// run_list(c)
bool chain_in_bg(command *c)
{ // thank you section code :)
    while (c->link != TYPE_SEQUENCE && c->link != TYPE_BACKGROUND)
    {
        c = c->next;
    }
    return c->link == TYPE_BACKGROUND;
}

// helper function for run_list that essentially just processes every command in list, takes care of
//  pipes, cd, and also keeps track of state! The state is non-zero if there is any errors or trouble.
// also creates pipes and decides if command is executable based on operator logic
command *rl_helper(command *c)
{
    command *firstpipe = nullptr;
    int status;
    while (c != nullptr)
    {
        if (c->executable)
        {
            if (c->link == TYPE_PIPE && c->next)
            {
                int npipefd[2];
                if (pipe(npipefd) < 0)
                {
                    perror("pipe");
                    _exit(1);
                }
                c->next->pipefd[0] = npipefd[0];
                c->pipefd[1] = npipefd[1];
                firstpipe = firstpipe ? firstpipe : c;
            }
            if (!c->args.empty() && c->args[0] == "cd")
            {
                c->state = abs(chdir(c->args[1].c_str()));
            }
            else
            {
                c->run();
                if (c->link == TYPE_BACKGROUND)
                {
                    return c->next;
                }
                if (c->link != TYPE_PIPE || c->next == nullptr || c->next->link != TYPE_PIPE)
                {
                    waitpid(c->pid, &status, 0);
                    c->state = !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                    firstpipe = nullptr;
                }
            }
        }
        else
        {
            c->state = c->prev->state;
        }
        // we do not want to run the command if the || or && logic means we should not, so setting executable accordingly
        if ((c->next && ((c->link == TYPE_AND && c->state != 0) || (c->link == TYPE_OR && c->state == 0))))
        {
            c->next->executable = false;
        }

        c = c->next; // going next
    }
    // closing reads
    for (command *rclose = c; rclose != nullptr; rclose = rclose->next)
    {
        if (rclose->pipefd[0] != -1)
        {
            close(rclose->pipefd[0]);
            rclose->pipefd[0] = -1;
        }
    }
    return c;
}

void run_list(command *c)
{
    while (c != nullptr)
    {
        // run background chain
        if (chain_in_bg(c))
        {
            pid_t child_p = fork();
            assert(child_p >= 0);
            if (child_p == 0)
            {
                c = rl_helper(c);
                _exit(1);
            }
            // going to next
            while (c->link != TYPE_BACKGROUND)
            {
                c = c->next;
            }
            c = c->next;
        }
        else
        {
            c = rl_helper(c);
        }
    }
}

// parse_line(s)
//    Parse the command list in `s` and return it. Returns `nullptr` if
//    `s` is empty (only spaces).

command *parse_line(const char *s)
{
    shell_parser parser(s);
    command *ccur = nullptr;
    command *chead = nullptr;
    command *clast = nullptr;
    int redir = 0;

    for (auto it = parser.begin(); it != parser.end(); ++it)
    {
        switch (it.type())
        {
        case TYPE_NORMAL:
            if (redir == 0)
            {
                if (!ccur)
                {
                    ccur = new command;
                    if (clast)
                    {
                        clast->next = ccur;
                        ccur->prev = clast;
                    }
                    else
                    {
                        chead = ccur;
                    }
                }
                ccur->args.push_back(it.str());
            }
            else
            {
                if (redir == 1)
                    ccur->file_i = it.str();
                else if (redir == 2)
                    ccur->file_o = it.str();
                else if (redir == 3)
                    ccur->error_f = it.str();
                redir = 0;
            }
            break;
        case TYPE_REDIRECT_OP:
            if (it.str() == "<")
            {
                redir = 1;
            }
            else if (it.str() == ">")
            {
                redir = 2;
            }
            else if (it.str() == "2>")
            {
                redir = 3;
            }
            break;
        case TYPE_BACKGROUND:
        case TYPE_SEQUENCE:
        case TYPE_PIPE:
        case TYPE_AND:
        case TYPE_OR:
            clast = ccur;
            clast->link = it.type();
            ccur = nullptr;
            break;
        }
    }
    waitpid(-1, 0, WNOHANG);
    return chead;
}

int main(int argc, char *argv[])
{
    FILE *command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0)
    {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1)
    {
        command_file = fopen(argv[1], "rb");
        if (!command_file)
        {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file))
    {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet)
        {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr)
        {
            if (ferror(command_file) && errno == EINTR)
            {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            }
            else
            {
                if (ferror(command_file))
                {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n'))
        {
            if (command *c = parse_line(buf))
            {
                run_list(c);
                delete c;
            }
            bufpos = 0;
            needprompt = 1;
        }

        // zombie
        waitpid(-1, 0, WNOHANG);
    }

    return 0;
}