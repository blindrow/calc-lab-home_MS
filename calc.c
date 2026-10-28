// Mihails Semjonovs 251ADB194
// https://github.com/blindrow/calc-lab-home_MS
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: RTU Arithmetic Parser assignment; RTU calc-tests.zip; OpenAI ChatGPT (GPT-6).
//   Full implementation chat: https://chatgpt.com/share/6ae11a0b-1718-83ed-b3e8-d521aac217eb
// AI disclosure: ChatGPT generated the complete implementation at my request.
//   I also used ChatGPT for learning, review, and this readability revision.
//   The comments below explain the relevant design choices and AI assistance.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#define FIRST_NAME "mihails"
#define LAST_NAME "semjonovs"
#define STUDENT_ID "251ADB194"
#define READ_BLOCK 65536

typedef struct {
    double floating;
    long long integer;
    int is_float;
} Value;

typedef enum {
    T_END, T_NUMBER, T_PLUS, T_MINUS, T_STAR, T_SLASH,
    T_POWER, T_LEFT, T_RIGHT, T_INVALID
} TokenKind;

typedef struct {
    TokenKind kind;
    size_t position;       /* 1-based position in the whole input file. */
    Value number;
} Token;

typedef struct {
    Value value;
    size_t operator_pos;
    size_t right_pos;
} PowerPart;

typedef struct {
    char *line;            /* Text of the current line, ending in '\0'. */
    size_t length;
    size_t index;
    size_t file_base;      /* Bytes preceding this line in the input file. */
    Token token;
    int failed;
    int resource_failed;
    size_t error_pos;

    /* Stores exponentiation chains without deep recursion. */
    PowerPart *powers;
    size_t power_length;
    size_t power_capacity;
} Parser;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Line;

static Value int_value(long long n) {
    Value v = {0};
    v.integer = n;
    return v;
}

static Value float_value(double n) {
    Value v = {0};
    v.floating = n;
    v.is_float = 1;
    return v;
}

static double as_double(Value v) {
    return v.is_float ? v.floating : (double)v.integer;
}


// Keep the first error position so later errors cannot overwrite it.
static void fail_at(Parser *p, size_t position) {
    if (p->failed) {
        return;
    }

    p->failed = 1;
    p->error_pos = position;
}


/* AI (ChatGPT): tokenizer and absolute error positions.
 * Prompt excerpt: "Develop a complete, reliable C17 implementation of the
 * Arithmetic Parser assignment" (full implementation chat linked above).
 * A token's position is file_base + index + 1, so comment lines and newlines
 * still count. T_END marks the newline, or one past the file's last character.
 * This also gives the correct position for a missing operand or ')'.
 */
static void next_token(Parser *p) {
    size_t start;
    char saved;
    char *endptr;
    Token t = {0};

while (p->index < p->length) {
    char c = p->line[p->index];

    if (c != ' ' && c != '\t') {
        break;
    }

    ++p->index;
}





    start = p->index;
    t.position = p->file_base + start + 1;
    if (start == p->length) {
        t.kind = T_END;
    } else if (p->line[start] >= '0' && p->line[start] <= '9') {
        size_t end = start;
        int is_float = 0;
        while (end < p->length && p->line[end] >= '0' && p->line[end] <= '9')
            ++end;
        /* The dot belongs to a number only when another digit follows.
           Otherwise, 5. must report an error at the dot. */
        if (end + 1 < p->length && p->line[end] == '.' &&
            p->line[end + 1] >= '0' && p->line[end + 1] <= '9') {
            is_float = 1;
            end += 2;
            while (end < p->length && p->line[end] >= '0' && p->line[end] <= '9')
                ++end;
        }
        saved = p->line[end];
        p->line[end] = '\0';
        errno = 0;
        if (is_float) {
            double n = strtod(p->line + start, &endptr);
            if (errno == ERANGE && !isfinite(n))
                t.kind = T_INVALID; /* Oversized literal: unspecified case. */
            else {
                t.kind = T_NUMBER;
                t.number = float_value(n);
            }
        } else {
            long long n = strtoll(p->line + start, &endptr, 10);
            if (errno == ERANGE)
                t.kind = T_INVALID; /* Out-of-range literal: unspecified case. */
            else {
                t.kind = T_NUMBER;
                t.number = int_value(n);
            }
        }
        (void)endptr;  /* Format already validated by the scanner above. */
        p->line[end] = saved;
        p->index = end;
    } else {
        ++p->index;
        switch (p->line[start]) {
        case '+': t.kind = T_PLUS; break;
        case '-': t.kind = T_MINUS; break;
        case '/': t.kind = T_SLASH; break;
        case '(': t.kind = T_LEFT; break;
        case ')': t.kind = T_RIGHT; break;
        case '*':
            if (p->index < p->length && p->line[p->index] == '*') {
                ++p->index;
                t.kind = T_POWER;
            } else {
                t.kind = T_STAR;
            }
            break;
        default: t.kind = T_INVALID; break;
        }
    }
    p->token = t;
}

static int grow_powers(Parser *p, size_t needed) {
    size_t capacity = p->power_capacity ? p->power_capacity : 8;
    PowerPart *new_parts;
    if (needed <= p->power_capacity)
        return 1;
    if (needed > ((size_t)-1) / sizeof(*new_parts))
        return 0;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    new_parts = realloc(p->powers, capacity * sizeof(*new_parts));
    if (!new_parts)
        return 0;
    p->powers = new_parts;
    p->power_capacity = capacity;
    return 1;
}

/* Check for integer overflow before doing calculations. */
static long long signed_maximum(void) {
    return (long long)(~0ULL >> 1);
}

static int integer_arithmetic(long long a, long long b, TokenKind op,
                              long long *result) {
    long long max = signed_maximum();
    long long min = -max - 1;
    if (op == T_PLUS) {
        if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return 0;
        *result = a + b;
    } else if (op == T_MINUS) {
        if ((b < 0 && a > max + b) || (b > 0 && a < min + b)) return 0;
        *result = a - b;
    } else if (op == T_STAR) {
        if (a > 0) {
            if ((b > 0 && a > max / b) || (b < 0 && b < min / a)) return 0;
        } else if (a < 0) {
            if ((b > 0 && a < min / b) || (b < 0 && a < max / b)) return 0;
        }
        *result = a * b;
    } else if (op == T_SLASH) {
        if (b == 0 || (a == min && b == -1)) return 0;
        *result = a / b;  /* C17 truncates toward zero. */
    } else if (op == T_POWER) {
        long long value = 1;
        if (b < 0) return 0;
        /* Calculate powers using repeated squaring. */
        while (b > 0) {
            if ((b & 1LL) && !integer_arithmetic(value, a, T_STAR, &value))
                return 0;
            b /= 2;
            if (b > 0 && !integer_arithmetic(a, a, T_STAR, &a))
                return 0;
        }
        *result = value;
    } else {
        return 0;
    }
    return 1;
}

/* AI (ChatGPT): integer and floating-point arithmetic.
 * Prompt excerpt: "Integer and floating-point arithmetic" (see chat links).
 * Keep exact integer calculations when both operands are integers.
 * If either is a float, calculate in double. A zero divisor reports its
 * operand's start; an infinite or NaN result reports the operator's position.
 */
static Value evaluate(Parser *p, Value left, Value right, TokenKind op,
                      size_t operator_pos, size_t right_pos) {
    Value zero = int_value(0);
    if (op == T_SLASH &&
        ((right.is_float && right.floating == 0.0) ||
         (!right.is_float && right.integer == 0))) {
        fail_at(p, right_pos);
        return zero;
    }
    if (!left.is_float && !right.is_float) {
        long long result;
        if (op == T_POWER && right.integer < 0) {
            fail_at(p, right_pos);
            return zero;
        }
        if (!integer_arithmetic(left.integer, right.integer, op, &result)) {
            fail_at(p, operator_pos);  /* Other integer overflow is unspecified. */
            return zero;
        }
        return int_value(result);
    }
    {
        double a = as_double(left);
        double b = as_double(right);
        double result;
        switch (op) {
        case T_PLUS:   result = a + b; break;
        case T_MINUS:  result = a - b; break;
        case T_STAR:   result = a * b; break;
        case T_SLASH:  result = a / b; break;
        case T_POWER:  result = pow(a, b); break;
        default:       fail_at(p, operator_pos); return zero;
        }
        if (!isfinite(result)) {
            fail_at(p, operator_pos);
            return zero;
        }
        return float_value(result);
    }
}

/* AI (ChatGPT): recursive descent parser and right-associative powers.
 * Prompt excerpt: "A custom tokenizer and recursive descent parser".
 *
 * expression = term    { ('+' | '-') term }
 * term       = power   { ('*' | '/') power }
 * power      = primary { '**' primary }   (evaluate from right to left)
 * primary    = number | '(' expression ')'
 *
 * Separate functions enforce precedence: multiplication finishes before
 * addition. Powers are collected and evaluated backwards, so 2**3**2
 * means 2**(3**2). Parentheses recursively parse another expression.
 */
static Value parse_expression(Parser *p);

static Value parse_primary(Parser *p) {
    Value zero = int_value(0);
    Value value;
    if (p->failed) return zero;
    if (p->token.kind == T_NUMBER) {
        value = p->token.number;
        next_token(p);
        return value;
    }
    if (p->token.kind == T_LEFT) {
        next_token(p);
        value = parse_expression(p);
        if (p->failed) return zero;
        if (p->token.kind != T_RIGHT) {
            fail_at(p, p->token.position); /* Unexpected token or line end. */
            return zero;
        }
        next_token(p);
        return value;
    }
    /* Also handles unary operators, '()', two adjacent operators, and EOF. */
    fail_at(p, p->token.position);
    return zero;
}

static Value parse_power(Parser *p) {
    size_t base = p->power_length;
    size_t i;
    Value current = parse_primary(p);
    Value zero = int_value(0);
    if (p->failed) return zero;
    if (!grow_powers(p, base + 1)) {
        p->resource_failed = 1;
        fail_at(p, p->token.position);
        return zero;
    }
    p->powers[base].value = current;
    p->power_length = base + 1;
    while (p->token.kind == T_POWER) {
        size_t op_pos = p->token.position;
        size_t rhs_pos;
        next_token(p);
        rhs_pos = p->token.position;
        current = parse_primary(p);
        if (p->failed) {
            p->power_length = base;
            return zero;
        }
        if (!grow_powers(p, p->power_length + 1)) {
            p->resource_failed = 1;
            fail_at(p, rhs_pos);
            p->power_length = base;
            return zero;
        }
        p->powers[p->power_length].value = current;
        p->powers[p->power_length].operator_pos = op_pos;
        p->powers[p->power_length].right_pos = rhs_pos;
        ++p->power_length;
    }
    current = p->powers[p->power_length - 1].value;
    for (i = p->power_length - 1; i > base; --i) {
        current = evaluate(p, p->powers[i - 1].value, current, T_POWER,
                           p->powers[i].operator_pos, p->powers[i].right_pos);
        if (p->failed) break;
    }
    p->power_length = base;
    return current;
}

static Value parse_term(Parser *p) {
    Value left = parse_power(p);
    while (!p->failed &&
           (p->token.kind == T_STAR || p->token.kind == T_SLASH)) {
        TokenKind op = p->token.kind;
        size_t op_pos = p->token.position;
        size_t right_pos;
        Value right;
        next_token(p);
        right_pos = p->token.position;
        right = parse_power(p);
        if (p->failed) break;
        left = evaluate(p, left, right, op, op_pos, right_pos);
    }
    return left;
}

static Value parse_expression(Parser *p) {
    Value left = parse_term(p);
    while (!p->failed &&
           (p->token.kind == T_PLUS || p->token.kind == T_MINUS)) {
        TokenKind op = p->token.kind;
        size_t op_pos = p->token.position;
        size_t right_pos;
        Value right;
        next_token(p);
        right_pos = p->token.position;
        right = parse_term(p);
        if (p->failed) break;
        left = evaluate(p, left, right, op, op_pos, right_pos);
    }
    return left;
}
/* 1 = success, 0 = invalid expression, -1 = file or memory error. */
static int process_line(Line *line, size_t file_base, FILE *results,
                        size_t *error_position) {
    Parser p = {0};
    size_t i = 0;
    Value value;
    int result;
    while (i < line->length &&
           (line->data[i] == ' ' || line->data[i] == '\t')) ++i;



    // Blank lines and comment lines do not produce results.
    if (i == line->length) {
        return 1;
    }

    if (line->data[i] == '#') {
        return 1;
    }



    p.line = line->data;
    p.length = line->length;
    p.file_base = file_base;
    next_token(&p);
    value = parse_expression(&p);
    if (!p.failed && p.token.kind != T_END)
        fail_at(&p, p.token.position); /* Extra ')' or any trailing garbage. */
    if (p.resource_failed) {
        result = -1;
    } else if (p.failed) {
        *error_position = p.error_pos;
        result = 0;
    } else if (value.is_float) {
        result = fprintf(results, "%.15g\n", value.floating) < 0 ? -1 : 1;
    } else {
        result = fprintf(results, "%lld\n", value.integer) < 0 ? -1 : 1;
    }
    free(p.powers);
    return result;
}

/* AI (ChatGPT): handling large files and keeping only the first error.
 * Prompt excerpt: "very large files handled efficiently".
 * Read fixed-size blocks, but grow the line buffer when needed. Doubling
 * its capacity avoids copying the whole line for every new character.
 * Results go into tmpfile() first. If a later line is invalid, discard
 * those results and write only ERROR:<position> to the final output file.
 */
static int append_line(Line *line, const char *data, size_t amount) {
    size_t required;
    char *new_data;
    size_t capacity;
    if (amount > ((size_t)-1) - line->length - 1) return 0;
    required = line->length + amount + 1;
    if (required > line->capacity) {
        capacity = line->capacity ? line->capacity : 256;
        while (capacity < required) {
            if (capacity > ((size_t)-1) / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        new_data = realloc(line->data, capacity);
        if (!new_data) return 0;
        line->data = new_data;
        line->capacity = capacity;
    }
    memcpy(line->data + line->length, data, amount);
    line->length += amount;
    line->data[line->length] = '\0';
    return 1;
}

static int copy_results(FILE *source, FILE *target) {
    char buffer[READ_BLOCK];
    size_t count;
    rewind(source);
    while ((count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, count, target) != count) return 0;
    }
    return !ferror(source);
}

static char *joined_path(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    int separator = a && dir[a - 1] != '/';
    char *path;
    if (a > ((size_t)-1) - b - (size_t)separator - 1) return NULL;
    path = malloc(a + b + (size_t)separator + 1);
    if (!path) return NULL;
    memcpy(path, dir, a);
    if (separator) path[a++] = '/';
    memcpy(path + a, name, b + 1);
    return path;
}

static char *formatted(const char *fmt, const char *a,
                       const char *b, const char *c, const char *d) {
    int n = snprintf(NULL, 0, fmt, a, b, c, d);
    char *s;
    if (n < 0) return NULL;
    s = malloc((size_t)n + 1);
    if (!s) return NULL;
    snprintf(s, (size_t)n + 1, fmt, a, b, c, d);
    return s;
}

/* Return an owned basename, optionally removing the terminal .txt suffix. */
static char *path_base(const char *path, int remove_txt) {
    const char *start = path;
    size_t length = strlen(path);
    size_t end = length;
    size_t i;
    char *out;
    while (end && path[end - 1] == '/') --end;
    for (i = 0; i < end; ++i)
        if (path[i] == '/') start = path + i + 1;
    length = (size_t)(path + end - start);
    if (remove_txt && length >= 4 && !memcmp(start + length - 4, ".txt", 4))
        length -= 4;
    out = malloc(length + 1);
    if (!out) return NULL;
    memcpy(out, start, length);
    out[length] = '\0';
    return out;
}

static int process_file(const char *input_path, const char *output_dir,
                        const char *input_base) {
    FILE *input = NULL, *results = NULL, *output = NULL;
    char *filename = NULL, *output_path = NULL;
    char buffer[READ_BLOCK];
    Line line = {0};
    size_t file_base = 0, error_position = 0;
    size_t bytes;
    int outcome = -1;
    int have_expression_error = 0;
    input = fopen(input_path, "rb");
    if (!input) {
        fprintf(stderr, "Cannot open input '%s': %s\n", input_path, strerror(errno));
        goto cleanup;
    }
    results = tmpfile();
    if (!results) {
        fprintf(stderr, "Cannot make temporary results: %s\n", strerror(errno));
        goto cleanup;
    }
    for (;;) {
        size_t offset = 0;
        bytes = fread(buffer, 1, sizeof(buffer), input);
        while (offset < bytes) {
            const char *newline = memchr(buffer + offset, '\n', bytes - offset);
            size_t segment = newline ? (size_t)(newline - (buffer + offset))
                                     : bytes - offset;
            int status;
            if (!append_line(&line, buffer + offset, segment)) {
                fprintf(stderr, "Out of memory reading '%s'\n", input_path);
                goto cleanup;
            }
            offset += segment;
            if (!newline) break;
            status = process_line(&line, file_base, results, &error_position);
            if (status == -1) {
                fprintf(stderr, "Cannot write temporary results for '%s'\n", input_path);
                goto cleanup;
            }
            if (status == 0) {
                have_expression_error = 1;
                goto read_done;
            }
            file_base += line.length + 1;  /* Includes the newline, even for a blank line. */
            line.length = 0;
            ++offset;
        }
        if (bytes < sizeof(buffer)) {
            if (ferror(input)) {
                fprintf(stderr, "Cannot read input '%s'\n", input_path);
                goto cleanup;
            }
            break;
        }
    }



    // Process the last line even if the file has no final newline.
    if (line.length) {
        int status = process_line(&line, file_base, results, &error_position);
        if (status == -1) {
            fprintf(stderr, "Cannot write temporary results for '%s'\n", input_path);
            goto cleanup;
        }
        if (status == 0) have_expression_error = 1;
    }
read_done:
    filename = formatted("%s_%s_%s_%s.txt", input_base,
                         FIRST_NAME, LAST_NAME, STUDENT_ID);
    if (!filename) goto memory_error;
    output_path = joined_path(output_dir, filename);
    if (!output_path) goto memory_error;
    output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "Cannot write output '%s': %s\n", output_path, strerror(errno));
        goto cleanup;
    }
    if (have_expression_error) {
        if (fprintf(output, "ERROR:%zu\n", error_position) < 0) {
            fprintf(stderr, "Cannot write output '%s'\n", output_path);
            goto cleanup;
        }
    } else if (fflush(results) == EOF || !copy_results(results, output)) {
        fprintf(stderr, "Cannot transfer results into '%s'\n", output_path);
        goto cleanup;
    }
    if (fclose(output) == EOF) {
        output = NULL;
        fprintf(stderr, "Cannot close output '%s': %s\n", output_path, strerror(errno));
        goto cleanup;
    }
    output = NULL;
    outcome = 0;
    goto cleanup;
memory_error:
    fprintf(stderr, "Out of memory creating an output path\n");
cleanup:
    if (output) fclose(output);
    if (results) fclose(results);
    if (input) fclose(input);
    free(line.data);
    free(filename);
    free(output_path);
    return outcome;
}

static int ensure_directory(const char *path) {
    struct stat info;
    if (mkdir(path, 0777) == 0) return 1;
    if (errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode))   // An existing directory is fine, but a regular file is not.
        return 1;
    fprintf(stderr, "Cannot create output directory '%s': %s\n",
            path, strerror(errno));
    return 0;
}

static int has_txt_suffix(const char *s) {
    size_t n = strlen(s);

    if (n < 4) {

        return 0;
    }



    if (memcmp(s + n - 4, ".txt", 4) == 0) {
        return 1;

    }



    return 0;
}

/* AI (ChatGPT): directory mode and command-line options.
 * Prompt excerpt: "Correct handling of command-line arguments".
 * Collect input names before generating results. Otherwise output files
 * placed in the input folder could accidentally become new inputs.
 * Sorting names also makes directory processing predictable.
 */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} FileList;

static void free_files(FileList *files) {
    size_t i;
    for (i = 0; i < files->count; ++i) free(files->items[i]);
    free(files->items);
}

static int add_file(FileList *files, const char *name) {
    char **new_items;
    char *copy;
    size_t length = strlen(name);
    size_t capacity = files->capacity ? files->capacity : 16;
    if (files->count == ((size_t)-1)) return 0;
    if (files->count == files->capacity) {
        if (files->capacity > ((size_t)-1) / (2 * sizeof(*new_items)))
            capacity = files->count + 1;
        else if (files->capacity)
            capacity *= 2;
        if (capacity > ((size_t)-1) / sizeof(*new_items)) return 0;
        new_items = realloc(files->items, capacity * sizeof(*new_items));
        if (!new_items) return 0;
        files->items = new_items;
        files->capacity = capacity;
    }
    if (length == ((size_t)-1)) return 0;
    copy = malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, name, length + 1);
    files->items[files->count++] = copy;
    return 1;
}

static int compare_filenames(const void *a, const void *b) {
    const char *const *x = a;
    const char *const *y = b;
    return strcmp(*x, *y);
}

static void usage(void) {
    fprintf(stderr, "Usage: calc [-d DIR | --dir DIR] "
                    "[-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *input_path = NULL, *input_dir = NULL, *out_arg = NULL;
    char *base = NULL, *default_dir = NULL;
    const char *output_dir;
    DIR *directory = NULL;
    FileList files = {0};
    int rc = 1;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-d") || !strcmp(arg, "--dir")) {
            if (input_dir || ++i == argc || argv[i][0] == '\0' ||
                argv[i][0] == '-') {
                usage();
                goto done;
            }
            input_dir = argv[i];
        } else if (!strcmp(arg, "-o") || !strcmp(arg, "--output-dir")) {
            if (out_arg || ++i == argc || argv[i][0] == '\0' ||
                argv[i][0] == '-') {
                usage();
                goto done;
            }
            out_arg = argv[i];
        } else if (arg[0] == '-' || input_path) {
            usage();
            goto done;
        } else {
            input_path = arg;
        }
    }
    if ((input_path != NULL) == (input_dir != NULL)) {
        usage();
        goto done;
    }
    if (input_dir) {
        directory = opendir(input_dir);
        if (!directory) {
            fprintf(stderr, "Cannot open input directory '%s': %s\n",
                    input_dir, strerror(errno));
            goto done;
        }
    }
    base = path_base(input_dir ? input_dir : input_path, input_dir == NULL);
    if (!base) {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }
    if (out_arg) {
        output_dir = out_arg;
    } else {
        default_dir = formatted("%s_%s_%s", base, FIRST_NAME, STUDENT_ID, "");
        if (!default_dir) {
            fprintf(stderr, "Out of memory\n");
            goto done;
        }
        output_dir = default_dir;
    }
    if (!ensure_directory(output_dir)) goto done;
    if (input_path) {
        rc = process_file(input_path, output_dir, base);
    } else {
        struct dirent *entry;
        size_t k;
        rc = 0;
        for (;;) {
            char *full_path;
            struct stat info;
            errno = 0;
            entry = readdir(directory);
            if (!entry) {
                if (errno != 0) {
                    fprintf(stderr, "Cannot read input directory '%s': %s\n",
                            input_dir, strerror(errno));
                    rc = 1;
                }
                break;
            }
            if (!has_txt_suffix(entry->d_name)) continue;
            full_path = joined_path(input_dir, entry->d_name);
            if (!full_path) {
                fprintf(stderr, "Out of memory\n");
                rc = 1;
                break;
            }
            if (stat(full_path, &info) == 0 && S_ISREG(info.st_mode)) {
                if (!add_file(&files, entry->d_name)) {
                    fprintf(stderr, "Out of memory\n");
                    rc = 1;
                }
            }
            free(full_path);
            if (rc) break;
        }
        /* Close the input directory before writing output files. */
        closedir(directory);
        directory = NULL;
        if (!rc) {
            if (files.count > 1)
                qsort(files.items, files.count, sizeof(*files.items),
                      compare_filenames);
            for (k = 0; k < files.count; ++k) {
                char *full_path = joined_path(input_dir, files.items[k]);
                char *file_base = path_base(files.items[k], 1);
                if (!full_path || !file_base) {
                    fprintf(stderr, "Out of memory\n");
                    free(full_path);
                    free(file_base);
                    rc = 1;
                    break;
                }
                if (process_file(full_path, output_dir, file_base)) {
                    rc = 1;
                }
                free(file_base);
                free(full_path);
                if (rc) break;
            }
        }
    }
done:
    if (directory) closedir(directory);
    free_files(&files);
    free(default_dir);
    free(base);
    return rc;
}
