# 📝 **Editor Colaborativo PPD**

> Editor de texto colaborativo com interface gráfica **GTK+3**, comunicação entre processos via **MPI**, e paralelismo com **OpenMP**.  
> Projeto desenvolvido para a disciplina de **Programação Paralela e Distribuída (PPD)**.

---

## 📌 **Visão Geral**

Este projeto simula um ambiente colaborativo onde múltiplos usuários podem editar um documento em tempo real, com:

- ✏️ **Edição de linhas com bloqueio exclusivo**
- 💬 **Chat entre usuários**
- 📜 **Log de atividades**
- 🎨 **Interface amigável com GTK**

Cada processo representa um editor, e a sincronização é feita via **MPI**, garantindo que **nenhuma linha seja editada simultaneamente por mais de um usuário**.

---

## 🧱 **Estrutura do Projeto**






---

## 🚀 **Funcionalidades**

- ✅ MPI: Envio e recebimento de mensagens entre processos
- ✅ Sistema de bloqueio por linha (lock/unlock)
- ✅ Interface com GTK+3
- ✅ Chat colaborativo entre usuários
- ✅ Log com timestamps
- ✅ Suporte a OpenMP para testes paralelos
- ✅ Interface visual com destaques de cores

---

## 🧪 **Pré-requisitos**

Instale os seguintes pacotes antes de compilar:

```bash
sudo apt update
sudo apt install libgtk-3-dev mpich build-essential
```
## ⚙️ Compilação

Compile o projeto com o seguinte comando:

```bash
mpicc -fopenmp `pkg-config --cflags gtk+-3.0` -o co-write.o co-write.c -fopenmp `pkg-config --libs gtk+-3.0`

```

## ▶️ Execução
```
Execute com mpirun, especificando o número de processos (usuários):

mpirun -np 3 ./co-write.o
```



## 💡 Como Usar
   Selecione uma linha usando o campo numérico.
   Clique em "Editar Linha" para solicitar o bloqueio.
   Edite o conteúdo da linha (se o bloqueio for concedido).
   Clique em "Commit Linha" para salvar e liberar a linha.
   Use o campo de chat à direita para se comunicar.
   Acompanhe o log de atividades ao final da tela.


## 🎨 Destaques Visuais

🟩 Verde	Linha que você está editando

🟥 Vermelho	Linha sendo editada por outro usuário

Além disso:
    O log mostra ações como bloqueios, commits e liberações.
    O chat está sempre visível no lado direito da interface.
