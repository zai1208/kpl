#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void process_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // 1. INCLUDE directive
        if (strncmp(line, "INCLUDE", 7) == 0) {
            char inc_file[128];
            sscanf(line + 8, "%s", inc_file);
            process_file(inc_file);
        }
        // 2. PROC definition
        else if (strstr(line, "PROC")) {
            char name[128];
            sscanf(line, "PROC %127[^(]", name);
            printf("\nglobal %s\n%s:\n    push rbp\n    mov rbp, rsp\n", name, name);
        }
        // 3. ASM block
        else if (strstr(line, "ASM")) {
            while (fgets(line, sizeof(line), f) && !strchr(line, '}')) {
                printf("    %s", line);
            }
        }
        // 4. RETURN
        else if (strstr(line, "RETURN")) {
            printf("    mov rax, 0\n    pop rbp\n    ret\n");
        }
        // 5. FUNCTION CALL (Flat)
        else if (strchr(line, '(') && !strstr(line, "PROC") && !strstr(line, "#")) {
            char func_name[64], args[128];
            sscanf(line, "%63[^(](%127[^)])", func_name, args);
            
            char *token = strtok(args, ",");
            int i = 0;
            const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            
            while (token != NULL) {
                while (*token == ' ') token++; // Trim
                printf("    mov %s, %s\n", regs[i++], token);
                token = strtok(NULL, ",");
            }
            printf("    call %s\n", func_name);
        }
    }
    fclose(f);
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    process_file(argv[1]);
    return 0;
}
