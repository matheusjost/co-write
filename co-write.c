#include <gtk/gtk.h>
#include <mpi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define MAX_TEXT_SIZE 10000
#define MAX_LINES 255
#define MAX_LINE_LENGTH 256
#define MAX_USERNAME 50
#define MAX_MESSAGE 256

// Tipos de mensagens MPI
#define MSG_LINE_UPDATE 1
#define MSG_LINE_LOCK_REQUEST 2
#define MSG_LINE_LOCK_GRANTED 3
#define MSG_LINE_LOCK_DENIED 4
#define MSG_LINE_UNLOCK 5
#define MSG_CHAT 6
#define MSG_LOG_ENTRY 7


typedef struct {
    int locked_by;                    // -1 = livre, >= 0 = rank do usuário
    char owner_name[MAX_USERNAME];
} LineInfo;

typedef struct {
    GtkWidget *window;
    GtkWidget *text_view;
    GtkTextBuffer *text_buffer;
    GtkWidget *status_label;
    GtkWidget *line_spin;
    GtkWidget *edit_button;
    GtkWidget *commit_button;
    GtkWidget *log_view;
    GtkTextBuffer *log_buffer;
    GtkWidget *chat_view;
    GtkTextBuffer *chat_buffer;
    GtkWidget *chat_entry;
    
    gboolean p_update;                // Flag para updates programáticos
    char username[MAX_USERNAME];
    int rank;
    int size;
    int editing_line;                 // -1 = não editando, >= 0 = linha sendo editada
    LineInfo lines[MAX_LINES];
    char line_backup[MAX_LINE_LENGTH]; // backup da linha antes de editar
} EditorData;

typedef struct {
    int type;
    int line_number;
    char content[MAX_LINE_LENGTH];
    int sender_rank;
    char sender_name[MAX_USERNAME];
} Message;

typedef struct {
    EditorData *editor;
    Message msg;
} UpdateData;

typedef struct {
    int line_number;
    char content[MAX_LINE_LENGTH];
} OMPLineData;

EditorData *g_editor = NULL;
pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t receiver_thread;
int running = 1;

const char *word_bank[] = {
    "thread", "deadlock", "race", "speedup", "pragma", "private", "shared", "reduction", "critical", "atomic", "barrier", "schedule", "dynamic", "static", "chunk", "nowait", "master", "single", "sections", "rank", "size", "send", "recv", "broadcast"
};
const int word_bank_size = 24;

/**
 * Adiciona uma entrada no log com timestamp
 */
void append_log(EditorData *editor, const char *message) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(editor->log_buffer, &end);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[20];
    strftime(timestamp, 20, "%H:%M:%S", tm_info);

    char log_entry[512];
    sprintf(log_entry, "[%s] %s\n", timestamp, message);

    gtk_text_buffer_insert(editor->log_buffer, &end, log_entry, -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(editor->log_buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(editor->log_view), mark);
}

/**
 * Adiciona uma mensagem no chat
 */
void append_chat(EditorData *editor, const char *sender, const char *message) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(editor->chat_buffer, &end);

    char chat_entry[512];
    sprintf(chat_entry, "%s: %s\n", sender, message);

    gtk_text_buffer_insert(editor->chat_buffer, &end, chat_entry, -1);

    GtkTextMark *mark = gtk_text_buffer_get_insert(editor->chat_buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(editor->chat_view), mark);
}

/**
 * Gera conteúdo aleatório para uma linha usando OpenMP
 */
void generate_line_content_omp(char *buffer, int thread_id) {
    buffer[0] = '\0';
    int num_words = 5 + (rand() % 10);

    for (int i = 0; i < num_words; i++) {
        if (i > 0) strcat(buffer, " ");
        int word_idx = rand() % word_bank_size;
        strcat(buffer, word_bank[word_idx]);
    }
}

/**
 * Obtém o conteúdo de uma linha específica (após o identificador)
 */
char *get_line_content(GtkTextBuffer *buffer, int line_num) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buffer, &start, line_num);

    // Encontra o início do conteúdo (após o '|')
    while (!gtk_text_iter_ends_line(&start)) {
        gunichar ch = gtk_text_iter_get_char(&start);
        if (ch == '|') {
            gtk_text_iter_forward_char(&start);
            if (gtk_text_iter_get_char(&start) == ' ') {
                gtk_text_iter_forward_char(&start);
            }
            break;
        }
        gtk_text_iter_forward_char(&start);
    }

    end = start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }
    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

/**
 * Define o conteúdo de uma linha específica
 */
void set_line_content(GtkTextBuffer *buffer, int line_num, const char *text) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(buffer, &start, line_num);

    GtkTextIter content_start = start;
    // Encontra o início do conteúdo (após o '|')
    while (!gtk_text_iter_ends_line(&content_start)) {
        gunichar ch = gtk_text_iter_get_char(&content_start);
        if (ch == '|') {
            gtk_text_iter_forward_char(&content_start);
            if (gtk_text_iter_get_char(&content_start) == ' ') {
                gtk_text_iter_forward_char(&content_start);
            }
            break;
        }
        gtk_text_iter_forward_char(&content_start);
    }

    end = content_start;
    if (!gtk_text_iter_ends_line(&end)) {
        gtk_text_iter_forward_to_line_end(&end);
    }

    gtk_text_buffer_delete(buffer, &content_start, &end);
    gtk_text_buffer_insert(buffer, &content_start, text, -1);
}

/**
 * Destaca uma linha com cor baseada no status de bloqueio
 */
void highlight_line(EditorData *editor, int line_num) {
    GtkTextIter buffer_start, buffer_end;
    gtk_text_buffer_get_start_iter(editor->text_buffer, &buffer_start);
    gtk_text_buffer_get_end_iter(editor->text_buffer, &buffer_end);
    gtk_text_buffer_remove_all_tags(editor->text_buffer, &buffer_start, &buffer_end);

    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(editor->text_buffer, &start, line_num);
    end = start;
    gtk_text_iter_forward_line(&end);

    if (editor->lines[line_num].locked_by == editor->rank) {
        // Verde para linha editada pelo usuário atual
        GtkTextTag *tag = gtk_text_buffer_create_tag(editor->text_buffer, NULL,
                                                    "background", "#90EE90", NULL);
        gtk_text_buffer_apply_tag(editor->text_buffer, tag, &start, &end);
    } else if (editor->lines[line_num].locked_by >= 0) {
        // Rosa para linha editada por outro usuário
        GtkTextTag *tag = gtk_text_buffer_create_tag(editor->text_buffer, NULL,
                                                    "background", "#FFB6C1", NULL);
        gtk_text_buffer_apply_tag(editor->text_buffer, tag, &start, &end);
    }
}

/**
 * Atualiza o status na interface
 */
void update_status(GtkWidget *widget, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line >= 0) {
        char status[256];
        sprintf(status, "Editando linha %d", editor->editing_line + 1);
        gtk_label_set_text(GTK_LABEL(editor->status_label), status);
    } else {
        int selected_line = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor->line_spin)) - 1;
        if (editor->lines[selected_line].locked_by >= 0) {
            char status[256];
            sprintf(status, "Linha %d bloqueada por %s",
                    selected_line + 1, editor->lines[selected_line].owner_name);
            gtk_label_set_text(GTK_LABEL(editor->status_label), status);
        } else {
            gtk_label_set_text(GTK_LABEL(editor->status_label), "Selecione uma linha para editar");
        }
    }
}

/**
 * Processa atualizações da interface baseadas em mensagens MPI
 */
gboolean update_interface(gpointer data) {
    UpdateData *update = (UpdateData *)data;
    EditorData *editor = update->editor;
    Message *msg = &update->msg;

    pthread_mutex_lock(&update_mutex);

    switch (msg->type) {
        case MSG_LINE_UPDATE:
            editor->p_update = TRUE;
            set_line_content(editor->text_buffer, msg->line_number, msg->content);
            
            char update_msg[256];
            sprintf(update_msg, "%s atualizou linha %d", msg->sender_name, msg->line_number + 1);
            append_log(editor, update_msg);
            editor->p_update = FALSE;
            break;

        case MSG_LINE_LOCK_REQUEST:
            if (editor->lines[msg->line_number].locked_by == -1) {
                // Concede o bloqueio
                editor->lines[msg->line_number].locked_by = msg->sender_rank;
                strcpy(editor->lines[msg->line_number].owner_name, msg->sender_name);

                Message reply;
                reply.type = MSG_LINE_LOCK_GRANTED;
                reply.line_number = msg->line_number;
                MPI_Send(&reply, sizeof(Message), MPI_BYTE, msg->sender_rank, 0, MPI_COMM_WORLD);

                char log_msg[256];
                sprintf(log_msg, "%s começou a editar linha %d", msg->sender_name, msg->line_number + 1);
                append_log(editor, log_msg);
                highlight_line(editor, msg->line_number);
            } else {
                // Nega o bloqueio
                Message reply;
                reply.type = MSG_LINE_LOCK_DENIED;
                reply.line_number = msg->line_number;
                reply.sender_rank = editor->lines[msg->line_number].locked_by;
                strcpy(reply.sender_name, editor->lines[msg->line_number].owner_name);
                MPI_Send(&reply, sizeof(Message), MPI_BYTE, msg->sender_rank, 0, MPI_COMM_WORLD);
            }
            break;

        case MSG_LINE_LOCK_GRANTED:
            editor->editing_line = msg->line_number;
            editor->lines[msg->line_number].locked_by = editor->rank;
            strcpy(editor->lines[msg->line_number].owner_name, editor->username);

            // Backup da linha atual
            char *current = get_line_content(editor->text_buffer, msg->line_number);
            strncpy(editor->line_backup, current, MAX_LINE_LENGTH - 1);
            g_free(current);

            // Atualiza interface
            gtk_widget_set_sensitive(editor->edit_button, FALSE);
            gtk_widget_set_sensitive(editor->commit_button, TRUE);
            gtk_widget_set_sensitive(editor->line_spin, FALSE);
            gtk_text_view_set_editable(GTK_TEXT_VIEW(editor->text_view), TRUE);

            highlight_line(editor, msg->line_number);
            update_status(NULL, editor);

            // Posiciona cursor após o identificador
            GtkTextIter iter;
            gtk_text_buffer_get_iter_at_line(editor->text_buffer, &iter, msg->line_number);
            while (!gtk_text_iter_ends_line(&iter)) {
                if (gtk_text_iter_get_char(&iter) == '|') {
                    gtk_text_iter_forward_char(&iter);
                    if (gtk_text_iter_get_char(&iter) == ' ') {
                        gtk_text_iter_forward_char(&iter);
                    }
                    break;
                }
                gtk_text_iter_forward_char(&iter);
            }
            gtk_text_buffer_place_cursor(editor->text_buffer, &iter);
            break;

        case MSG_LINE_LOCK_DENIED:
            char denied_msg[256];
            sprintf(denied_msg, "Linha %d já está sendo editada por %s",
                    msg->line_number + 1, msg->sender_name);
            append_log(editor, denied_msg);
            update_status(NULL, editor);
            break;

        case MSG_LINE_UNLOCK:
            if (editor->lines[msg->line_number].locked_by == msg->sender_rank) {
                editor->lines[msg->line_number].locked_by = -1;
                strcpy(editor->lines[msg->line_number].owner_name, "");

                char unlock_msg[256];
                sprintf(unlock_msg, "%s liberou linha %d", msg->sender_name, msg->line_number + 1);
                append_log(editor, unlock_msg);

                highlight_line(editor, msg->line_number);
                update_status(NULL, editor);
            }
            break;

        case MSG_CHAT:
            append_chat(editor, msg->sender_name, msg->content);
            break;

        case MSG_LOG_ENTRY:
            append_log(editor, msg->content);
            break;
    }

    pthread_mutex_unlock(&update_mutex);
    g_free(update);
    return FALSE;
}

/**
 * Thread para receber mensagens MPI
 */
void *mpi_receiver(void *arg) {
    EditorData *editor = (EditorData *)arg;
    Message msg;
    MPI_Status status;

    while (running) {
        int flag;
        MPI_Iprobe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            MPI_Recv(&msg, sizeof(Message), MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

            UpdateData *update = g_new(UpdateData, 1);
            update->editor = editor;
            update->msg = msg;
            g_idle_add(update_interface, update);
        }

        usleep(10000); // 10ms
    }

    return NULL;
}

/**
 * Callback para geração de dados com OpenMP
 */
void on_generate_data_omp(GtkWidget *button, gpointer data) {
    EditorData *editor = (EditorData *)data;
    gtk_widget_set_sensitive(button, FALSE);

    char log_msg[256];
    sprintf(log_msg, "Iniciando geração paralela com %d threads OpenMP...", omp_get_max_threads());
    append_log(editor, log_msg);

    OMPLineData *generated_data = calloc(MAX_LINES, sizeof(OMPLineData));
    char (*log_messages)[256] = calloc(MAX_LINES, 256);
    int *log_flags = calloc(MAX_LINES, sizeof(int));

    #pragma omp parallel
    {
        unsigned int seed = time(NULL) + omp_get_thread_num();
        #pragma omp for schedule(dynamic)
        for (int i = 0; i < MAX_LINES; i++) {
            generated_data[i].line_number = i;
            generated_data[i].content[0] = '\0';
            
            int num_words = 5 + (rand_r(&seed) % 10);
            for (int w = 0; w < num_words; w++) {
                if (w > 0) strcat(generated_data[i].content, " ");
                int word_idx = rand_r(&seed) % word_bank_size;
                strcat(generated_data[i].content, word_bank[word_idx]);
            }
            
            sprintf(log_messages[i], "Thread %d gerou dado para linha %d com %d palavras",
                    omp_get_thread_num(), i + 1, num_words);
            log_flags[i] = 1;
        }
    }

    // Log das operações
    for (int i = 0; i < MAX_LINES; i++) {
        if (log_flags[i]) {
            append_log(editor, log_messages[i]);
        }
    }

    free(log_messages);
    free(log_flags);

    sprintf(log_msg, "Dados gerados. Enviando atualizações...");
    append_log(editor, log_msg);

    // Envia atualizações para todas as linhas não bloqueadas
    for (int i = 0; i < MAX_LINES; i++) {
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }

        if (editor->lines[generated_data[i].line_number].locked_by == -1) {
            Message update_msg;
            update_msg.type = MSG_LINE_UPDATE;
            update_msg.line_number = generated_data[i].line_number;
            update_msg.sender_rank = editor->rank;
            strcpy(update_msg.sender_name, editor->username);
            strcpy(update_msg.content, generated_data[i].content);

            // Envia para outros processos
            for (int j = 0; j < editor->size; j++) {
                if (j != editor->rank) {
                    MPI_Send(&update_msg, sizeof(Message), MPI_BYTE, j, 0, MPI_COMM_WORLD);
                }
            }

            // Atualiza interface local
            UpdateData *update = g_new(UpdateData, 1);
            update->editor = editor;
            update->msg = update_msg;
            g_idle_add(update_interface, update);
        }
    }

    free(generated_data);
    sprintf(log_msg, "Geração concluída!");
    append_log(editor, log_msg);

    gtk_widget_set_sensitive(button, TRUE);
    update_status(NULL, editor);
}

/**
 * Callback para solicitar edição de uma linha
 */
void on_edit_clicked(GtkWidget *button, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line >= 0) return; // Já está editando

    int line_num = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor->line_spin)) - 1;

    Message msg;
    msg.type = MSG_LINE_LOCK_REQUEST;
    msg.line_number = line_num;
    msg.sender_rank = editor->rank;
    strcpy(msg.sender_name, editor->username);

    char log_msg[256];
    sprintf(log_msg, "%s solicitou edição da linha %d", editor->username, line_num + 1);
    append_log(editor, log_msg);

    // Envia solicitação para outros processos
    for (int i = 0; i < editor->size; i++) {
        if (i != editor->rank) {
            MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }

    // Se é o único processo, concede automaticamente
    if (editor->size == 1) {
        UpdateData *update = g_new(UpdateData, 1);
        update->editor = editor;
        update->msg.type = MSG_LINE_LOCK_GRANTED;
        update->msg.line_number = line_num;
        g_idle_add(update_interface, update);
    }
}

/**
 * Callback para confirmar edição de uma linha
 */
void on_commit_clicked(GtkWidget *button, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line < 0) return; // Não está editando

    char *content = get_line_content(editor->text_buffer, editor->editing_line);

    // Envia atualização
    Message msg;
    msg.type = MSG_LINE_UPDATE;
    msg.line_number = editor->editing_line;
    msg.sender_rank = editor->rank;
    strcpy(msg.sender_name, editor->username);
    strncpy(msg.content, content, MAX_LINE_LENGTH - 1);

    for (int i = 0; i < editor->size; i++) {
        if (i != editor->rank) {
            MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }

    // Libera o bloqueio
    msg.type = MSG_LINE_UNLOCK;
    for (int i = 0; i < editor->size; i++) {
        if (i != editor->rank) {
            MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }

    // Atualiza estado local
    editor->lines[editor->editing_line].locked_by = -1;
    strcpy(editor->lines[editor->editing_line].owner_name, "");
    highlight_line(editor, editor->editing_line);

    char log_msg[256];
    sprintf(log_msg, "%s commitou linha %d", editor->username, editor->editing_line + 1);
    append_log(editor, log_msg);

    // Restaura interface
    editor->editing_line = -1;
    gtk_widget_set_sensitive(editor->edit_button, TRUE);
    gtk_widget_set_sensitive(editor->commit_button, FALSE);
    gtk_widget_set_sensitive(editor->line_spin, TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(editor->text_view), FALSE);

    update_status(NULL, editor);
    g_free(content);
}

/**
 * Callback para enviar mensagem no chat
 */
void on_chat_send(GtkWidget *entry, gpointer data) {
    EditorData *editor = (EditorData *)data;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));

    if (strlen(text) == 0) return;

    append_chat(editor, editor->username, text);

    Message msg;
    msg.type = MSG_CHAT;
    msg.sender_rank = editor->rank;
    strcpy(msg.sender_name, editor->username);
    strncpy(msg.content, text, MAX_LINE_LENGTH - 1);

    for (int i = 0; i < editor->size; i++) {
        if (i != editor->rank) {
            MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }

    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

/**
 * Handler para cliques no editor
 */
gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line >= 0) {
        gint x, y;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget),
                                             GTK_TEXT_WINDOW_WIDGET,
                                             event->x, event->y, &x, &y);

        GtkTextIter click_iter;
        gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &click_iter, x, y);

        int clicked_line = gtk_text_iter_get_line(&click_iter);

        // Bloqueia cliques fora da linha sendo editada
        if (clicked_line != editor->editing_line) {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Handler para teclas pressionadas
 */
gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line >= 0) {
        // Bloqueia navegação com teclas
        if (event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_Down ||
            event->keyval == GDK_KEY_Page_Up || event->keyval == GDK_KEY_Page_Down ||
            event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
            return TRUE;
        }

        GtkTextIter cursor;
        GtkTextMark *mark = gtk_text_buffer_get_insert(editor->text_buffer);
        gtk_text_buffer_get_iter_at_mark(editor->text_buffer, &cursor, mark);

        int cursor_line = gtk_text_iter_get_line(&cursor);

        // Reposiciona cursor se não está na linha sendo editada
        if (cursor_line != editor->editing_line) {
            GtkTextIter correct_pos;
            gtk_text_buffer_get_iter_at_line(editor->text_buffer, &correct_pos, editor->editing_line);

            while (!gtk_text_iter_ends_line(&correct_pos)) {
                if (gtk_text_iter_get_char(&correct_pos) == '|') {
                    gtk_text_iter_forward_char(&correct_pos);
                    if (gtk_text_iter_get_char(&correct_pos) == ' ') {
                        gtk_text_iter_forward_char(&correct_pos);
                    }
                    break;
                }
                gtk_text_iter_forward_char(&correct_pos);
            }

            gtk_text_buffer_place_cursor(editor->text_buffer, &correct_pos);
            return TRUE;
        }

        // Protege o identificador da linha
        GtkTextIter line_start;
        gtk_text_buffer_get_iter_at_line(editor->text_buffer, &line_start, editor->editing_line);

        int cursor_offset = gtk_text_iter_get_line_offset(&cursor);

        GtkTextIter iter = line_start;
        int pipe_pos = -1;
        int pos = 0;
        while (!gtk_text_iter_ends_line(&iter)) {
            if (gtk_text_iter_get_char(&iter) == '|') {
                pipe_pos = pos;
                break;
            }
            gtk_text_iter_forward_char(&iter);
            pos++;
        }

        if (pipe_pos >= 0 && cursor_offset <= pipe_pos + 1) {
            if ((event->keyval == GDK_KEY_BackSpace && cursor_offset <= pipe_pos + 2) ||
                (event->keyval == GDK_KEY_Delete && cursor_offset <= pipe_pos + 1)) {
                return TRUE;
            }
            if (cursor_offset <= pipe_pos) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * Handler para inserção de texto
 */
void on_insert_text(GtkTextBuffer *buffer, GtkTextIter *location,
                   gchar *text, gint len, gpointer data) {
    EditorData *editor = (EditorData *)data;

    if (editor->editing_line >= 0) {
        int insert_line = gtk_text_iter_get_line(location);

        // Bloqueia inserção fora da linha sendo editada
        if (insert_line != editor->editing_line && !editor->p_update) {
            g_signal_stop_emission_by_name(buffer, "insert-text");
        }
    }
}

/**
 * Handler para fechamento da janela
 */
void on_window_destroy(GtkWidget *widget, gpointer data) {
    EditorData *editor = (EditorData *)data;

    // Libera linha se estiver editando
    if (editor->editing_line >= 0) {
        Message msg;
        msg.type = MSG_LINE_UNLOCK;
        msg.line_number = editor->editing_line;
        msg.sender_rank = editor->rank;

        for (int i = 0; i < editor->size; i++) {
            if (i != editor->rank) {
                MPI_Send(&msg, sizeof(Message), MPI_BYTE, i, 0, MPI_COMM_WORLD);
            }
        }
    }

    running = 0;
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    // Inicialização MPI
    MPI_Init(&argc, &argv);

    EditorData editor;
    MPI_Comm_rank(MPI_COMM_WORLD, &editor.rank);
    MPI_Comm_size(MPI_COMM_WORLD, &editor.size);

    // Configuração inicial
    if (editor.rank == 0) {
        strcpy(editor.username, "MASTER");
    } else {
        sprintf(editor.username, "Usuário %d", editor.rank);
    }
    editor.editing_line = -1;

    // Inicializa array de linhas
    for (int i = 0; i < MAX_LINES; i++) {
        editor.lines[i].locked_by = -1;
        strcpy(editor.lines[i].owner_name, "");
    }

    g_editor = &editor;

    // Inicialização GTK
    gtk_init(&argc, &argv);

    // Janela principal
    editor.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(editor.window), editor.username);
    gtk_window_set_default_size(GTK_WINDOW(editor.window), 900, 700);

    // Layout principal (horizontal)
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_add(GTK_CONTAINER(editor.window), main_box);

    // Lado esquerdo (editor + controles)
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(main_box), left_box, TRUE, TRUE, 5);

    // Controles
    GtkWidget *control_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(left_box), control_box, FALSE, FALSE, 5);

    gtk_box_pack_start(GTK_BOX(control_box), gtk_label_new("Linha:"), FALSE, FALSE, 5);

    editor.line_spin = gtk_spin_button_new_with_range(1, MAX_LINES, 1);
    gtk_box_pack_start(GTK_BOX(control_box), editor.line_spin, FALSE, FALSE, 5);

    editor.edit_button = gtk_button_new_with_label("Editar Linha");
    gtk_box_pack_start(GTK_BOX(control_box), editor.edit_button, FALSE, FALSE, 5);

    editor.commit_button = gtk_button_new_with_label("Commit Linha");
    gtk_widget_set_sensitive(editor.commit_button, FALSE);
    gtk_box_pack_start(GTK_BOX(control_box), editor.commit_button, FALSE, FALSE, 5);

    GtkWidget *generate_button = gtk_button_new_with_label("Gerar Dados OpenMP");
    gtk_box_pack_start(GTK_BOX(control_box), generate_button, FALSE, FALSE, 5);

    // Editor de texto
    GtkWidget *editor_frame = gtk_frame_new("Editor (Verde=você | Rosa=outro usuário)");
    gtk_box_pack_start(GTK_BOX(left_box), editor_frame, TRUE, TRUE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(editor_frame), scrolled);

    editor.text_view = gtk_text_view_new();
    editor.text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor.text_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(editor.text_view), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled), editor.text_view);

    // Texto inicial (numeração das linhas)
    char initial_text[MAX_LINES * 10];
    strcpy(initial_text, "");
    for (int i = 0; i < MAX_LINES; i++) {
        char line[10];
        int length_max_lines = floor(log10(MAX_LINES)) + 1;
        sprintf(line, "%0*d| \n", length_max_lines, i + 1);
        strcat(initial_text, line);
    }
    gtk_text_buffer_set_text(editor.text_buffer, initial_text, -1);

    // Log de atividades
    GtkWidget *log_frame = gtk_frame_new("Log de Atividades");
    gtk_box_pack_start(GTK_BOX(left_box), log_frame, FALSE, FALSE, 0);

    GtkWidget *log_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(log_scrolled), 100);
    gtk_container_add(GTK_CONTAINER(log_frame), log_scrolled);

    editor.log_view = gtk_text_view_new();
    editor.log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor.log_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(editor.log_view), FALSE);
    gtk_container_add(GTK_CONTAINER(log_scrolled), editor.log_view);

    // Chat (lado direito)
    GtkWidget *chat_frame = gtk_frame_new("Chat entre Usuários");
    gtk_box_pack_start(GTK_BOX(main_box), chat_frame, FALSE, FALSE, 5);

    GtkWidget *chat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(chat_frame), chat_box);
    gtk_widget_set_size_request(chat_box, 300, -1);

    GtkWidget *chat_scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(chat_box), chat_scrolled, TRUE, TRUE, 5);

    editor.chat_view = gtk_text_view_new();
    editor.chat_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor.chat_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(editor.chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(editor.chat_view), GTK_WRAP_WORD);
    gtk_container_add(GTK_CONTAINER(chat_scrolled), editor.chat_view);

    editor.chat_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(editor.chat_entry), "Digite sua mensagem...");
    gtk_box_pack_start(GTK_BOX(chat_box), editor.chat_entry, FALSE, FALSE, 5);

    // Status
    editor.status_label = gtk_label_new("Selecione uma linha para editar");
    gtk_box_pack_start(GTK_BOX(left_box), editor.status_label, FALSE, FALSE, 5);

    g_signal_connect(editor.edit_button, "clicked", G_CALLBACK(on_edit_clicked), &editor);
    g_signal_connect(editor.commit_button, "clicked", G_CALLBACK(on_commit_clicked), &editor);
    g_signal_connect(editor.chat_entry, "activate", G_CALLBACK(on_chat_send), &editor);
    g_signal_connect(editor.text_view, "key-press-event", G_CALLBACK(on_key_press), &editor);
    g_signal_connect(editor.window, "destroy", G_CALLBACK(on_window_destroy), &editor);
    g_signal_connect(editor.line_spin, "value-changed", G_CALLBACK(update_status), &editor);
    g_signal_connect(editor.text_view, "button-press-event", G_CALLBACK(on_button_press), &editor);
    g_signal_connect(editor.text_buffer, "insert-text", G_CALLBACK(on_insert_text), &editor);
    g_signal_connect(generate_button, "clicked", G_CALLBACK(on_generate_data_omp), &editor);

    editor.p_update = FALSE;

    // Log inicial
    append_log(&editor, "Sistema iniciado");
    append_chat(&editor, "Sistema", "Chat iniciado. Todos os usuários podem conversar aqui.");

    update_status(NULL, &editor);
    gtk_widget_show_all(editor.window);

    // Inicia thread MPI
    pthread_create(&receiver_thread, NULL, mpi_receiver, &editor);

    // Loop principal GTK
    gtk_main();

    // Limpeza
    pthread_join(receiver_thread, NULL);
    MPI_Finalize();

    return 0;
}
