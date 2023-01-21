#include "so.h"
#include "tela.h"
#include <stdio.h>
#include <stdlib.h>

struct metricas_proc {
  //metricas principais
  int tempo_retorno;          //tempo entre criação e fim
  int tempo_bloq;             //tempo que passou bloqueado
  int tempo_exec;             //tempo que passou executando
  int tempo_pronto;     //tempo que passou no estado pronto
  int n_bloqs;                //numero de bloqueios por E/S
  int n_preemp;               //numero de bloqueios por preempção
  //metricas auxiliares
  int existe_desde;         //registro do tempo de cpu no momento da criação do proc
  int bloq_desde;           //momento que bloqueou pela ultima vez
  int exec_desde;           //momendo que entrou em exec pela ultima vez
  int pronto_desde;         //momento que desbloqueou pela ultima vez
};

struct metricas_so {
  //metricas principais
  int tempo_total;          //tempo total de exec
  int tempo_total_exec;     //tempo total em modos usuario ou supervisor
  int n_int;                //numero de interrupções
  //metricas auxiliares
  int exec_desde;           //momento que saiu do modo zumbi pela ultima vez
};

struct proc_t
{
  int id_proc;             // id do processo
  cpu_estado_t *cpue_proc; // estado da cpu do processo
  proc_estado_t est_proc;  // estado do processo
  mem_t *mem_proc;         // memoria do processo

  proc_bloq_t motivo_bloq;    // pq bloqueou
  int disp_bloq;              // qual disp bloqueou

  float tempo_expect;       //tempo esperado da próxima execução
  float tempo_exec;         //tempo que passou executando desde que recebeu processador

  metricas_proc m_proc;
  
  proc_t* prox;
};

struct so_t {
  contr_t *contr;       // o controlador do hardware
  bool paniquei;        // apareceu alguma situação intratável
  cpu_estado_t *cpue;   // cópia do estado da CPU

  proc_t* proc_exec;    // ponteiro pro processo atual
  proc_t* proc_bloq;    // inicio da fila de processos bloqueados
  proc_t* proc_prt;     // inicio da fila de processos prontos

  int quantum;                        //quantum
  int (*esc_ptr) (so_t*, proc_t**);   //ponteiro para o escalonador 

  metricas_so m_so;
};

// funções auxiliares
static void init_mem(so_t *self);
static void init_mem_p1(so_t *self, mem_t* mem_p1);
static void init_mem_p2(so_t *self, mem_t* mem_p2);
static void panico(so_t *self);
static void salva_metricas_proc(proc_t* proc);
static void salva_metricas_so(so_t* so);

//################################################ FUNÇÕES DE TROCA DE CONTEXTO #######################################
err_t salva_contexto_proc(so_t *self, proc_t* proc)
{
  //salva na tabela o contexto do processador
  cpue_copia(self->cpue, proc->cpue_proc);

  int aux;
  
  for (int i = 0; i < mem_tam(proc->mem_proc); i++){
    //le da mem do sistema
    if (mem_le(contr_mem(self->contr), i, &aux) == ERR_OK){
      //salva na tabela
      if (mem_escreve(proc->mem_proc, i, aux) == ERR_OK){
        continue;
      } 
    }
    return ERR_END_INV;
  }
  return ERR_OK;
}

err_t restaura_contexto_proc(so_t* self, proc_t* proc){
  //restaura o contexto da tabela
  cpue_copia(proc->cpue_proc, self->cpue);

  int aux;
  
  for (int i = 0; i < mem_tam(proc->mem_proc); i++){
    //le da mem da tabela de proc
    if (mem_le(proc->mem_proc, i, &aux) == ERR_OK){
      //salva na mem do sistema
      if (mem_escreve(contr_mem(self->contr), i, aux) == ERR_OK){
        continue;
      } 
    }
    return ERR_END_INV;
  }
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  return ERR_OK;
}

//################################################ FUNÇÕES DE ESTADO DE PROCESSO #######################################

void desbloqueia_proc(so_t* self, proc_t* proc){
  //debug
  t_printf("Desbloqueando proc %d", proc->id_proc);
  //metricas
  proc->m_proc.tempo_bloq += rel_agora(contr_rel(self->contr)) - proc->m_proc.bloq_desde;
  proc->m_proc.pronto_desde = rel_agora(contr_rel(self->contr));

  proc->est_proc = PRONTO;

  proc_t* aux = self->proc_prt;
  if (aux == NULL){           //fila de prontos ta vazia
    self->proc_prt = proc;
  }
  if (aux != NULL){
    while (aux->prox != NULL){
      aux = aux->prox;
    }                         //aux = fim da fila de prontos
    aux->prox = proc;         //coloca o processo desbloq no fim da fila de prontos
  } 

  proc_t* aux2 = self->proc_bloq;
  if (aux2->id_proc == proc->id_proc) {       //o processo que desbloqueou é o primeiro da fila
    self->proc_bloq = self->proc_bloq->prox;  //retira o processo da fila de bloqueados
  } else {
    while ((aux2->prox != NULL) && (aux2->prox->id_proc != proc->id_proc)){
      aux2 = aux2->prox;
    }
    aux2->prox = aux2->prox->prox;
  }

  proc->prox = NULL;
}

void bloqueia_proc(so_t* self, proc_bloq_t motivo, int disp){
  //debug
  t_printf("Bloqueando proc %d", self->proc_exec->id_proc);
  //metricas 
  self->proc_exec->m_proc.tempo_exec += rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.exec_desde;
  self->proc_exec->m_proc.bloq_desde = rel_agora(contr_rel(self->contr));
  self->proc_exec->m_proc.n_bloqs++;

  self->proc_exec->est_proc = BLOQ;     
  self->proc_exec->motivo_bloq = motivo;    //guarda o motivo do bloqueio
  self->proc_exec->disp_bloq = disp;        //guarda qual disp bloqueou

  self->proc_exec->tempo_expect += self->proc_exec->tempo_exec;
  self->proc_exec->tempo_expect /= 2;
  self->proc_exec->tempo_exec = 0.0;          //zera o tempo em que executou

  proc_t* aux = self->proc_bloq;
  if (aux == NULL) {                        //fila de bloqueados esta vazia
    self->proc_bloq = self->proc_exec;      //coloca o processo bloquado na fila 
  } else {
    while(aux->prox != NULL){
      aux = aux->prox;
    }
    aux->prox = self->proc_exec;          //coloca o processo que bloqueou no fim da fila de bloqueados
  }

  proc_t* aux2 = self->proc_prt;
  if (aux2 == self->proc_exec) {            //o processo que bloqueou é o primeiro da fila
    self->proc_prt = self->proc_prt->prox;  //retira o processo da fila de prontos
  } else {
    while ((aux2->prox != NULL) && (aux2->prox->id_proc != self->proc_exec->id_proc)){
      aux2 = aux2->prox;
    }
    aux2->prox = aux2->prox->prox;
  }

  self->proc_exec->prox = NULL;
}

//################################################ ESCALONADORES #######################################

int escalonador_curto(so_t *self, proc_t** novo){
  //debug
  t_printf("Chamado escalonador mais curto");
  proc_t* aux = self->proc_prt;

  if (self->proc_prt == NULL){      //todos os processos estão bloqueados... ou
    if (self->proc_bloq == NULL){   //acabaram os processos
      return -1;
    }  
    return 0;
  }
  else {                //tem pelo menos 1 processo pronto
    *novo = aux;
    while(aux->prox != NULL) {
      if (aux->tempo_expect < (*novo)->tempo_expect){ 
        //achou um mais curto
        *novo = aux;
      }
      aux = aux->prox;
    }
    return 1;
  }
}

int escalonador_round(so_t *self, proc_t** novo)
{
  //debug
  t_printf("Chamado escalonador circular");

  if (self->proc_prt == NULL){      //todos os processos estão bloqueados... ou
    if (self->proc_bloq == NULL){   //acabaram os processos
      return -1;
    }  
    return 0;
  }
  else {
    *novo = self->proc_prt;       //simplesmente pega o primeiro da fila
    return 1;
  }
}

//################################################ FUNÇÕES DE ESCALONAMENTO #######################################

void troca_processo(so_t* self){
  proc_t* novo = NULL;
  int esc = (*(self->esc_ptr))(self, &novo);

  //debug
/*
  if (novo != NULL) {
    t_printf("idx = %d", novo->id_proc);
  } else {
    t_printf("tudo bloq");
  }
*/
  if (esc == -1) { //acabaram os processos
    self->proc_exec = NULL;

    //atualiza as metricas
    if (cpue_modo(self->cpue) != zumbi){  //a cpu estava executando
      self->m_so.tempo_total_exec += rel_agora(contr_rel(self->contr)) - self->m_so.exec_desde;
    }
    self->m_so.tempo_total = rel_agora(contr_rel(self->contr));

    t_printf("Fim da execução.");
    panico(self);
    return;

  } else if (esc == 0) { //todos bloqueados
    self->proc_exec = NULL;

    //atualiza metricas
    if (cpue_modo(self->cpue) != zumbi){    //cpu está entrando em modo zumbi
      self->m_so.tempo_total_exec += rel_agora(contr_rel(self->contr)) - self->m_so.exec_desde;
    }

    cpue_muda_modo(self->cpue, zumbi);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);
    return;

  } else if (esc == 1) { //achou processo
    self->proc_exec = novo;

    //metricas
    self->proc_exec->m_proc.tempo_pronto += rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.pronto_desde;
    self->proc_exec->m_proc.exec_desde = rel_agora(contr_rel(self->contr));
    if (cpue_modo(self->cpue) == zumbi){    //cpu esta saindo do modo zumbi 
      self->m_so.exec_desde = rel_agora(contr_rel(self->contr));
    }

    restaura_contexto_proc(self, self->proc_exec);
    cpue_muda_modo(self->cpue, usuario);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);
    return;
  }
}

void verif_processos(so_t *self)
{
  // processo a ser verificado
  proc_t* proc_verif = self->proc_bloq; //inicio da fila de bloqueados

  while(proc_verif != NULL) { //processo existe
      if (proc_verif->motivo_bloq == rel) //motivo do bloq foi o relogio
      {
        desbloqueia_proc(self, proc_verif);
      }
      else if (es_pronto(contr_es(self->contr), proc_verif->disp_bloq, (acesso_t)proc_verif->motivo_bloq)) //motivo foi E/S
      { // ve se pode desbloquear
        desbloqueia_proc(self, proc_verif);
      }
    proc_verif = proc_verif->prox; //proximo processo
  }
}

//################################################ FUNÇÕES DE CRIAÇÃO E INICIALIZAÇÃO #######################################

proc_t* novo_proc(so_t* self, int id)
{
  proc_t* novo = malloc(sizeof(*novo));
  if (novo == NULL) return NULL;
  
  novo->cpue_proc = cpue_cria();
  novo->mem_proc = mem_cria(MEM_TAM);

  novo->id_proc = id;
  novo->est_proc = PRONTO;

  novo->tempo_expect = self->quantum;
  novo->tempo_exec = 0.0;

  novo->m_proc = (metricas_proc){0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  novo->prox = NULL;

  return novo;
}

so_t *so_cria(contr_t *contr)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->contr = contr;
  self->paniquei = false;
  self->cpue = cpue_cria();

  self->proc_bloq = NULL;
  self->proc_prt = novo_proc(self, 0);
  self->proc_exec = self->proc_prt;

  //aqui escolhe entre um quantum grande e um pequeno
  //self->quantum = 15;      //quantum grande
  self->quantum = 6;       //quantum pequeno

  //aqui escolhe qual escalonador será usado
  self->esc_ptr = &escalonador_round;     //escalonador circular
  //self->esc_ptr = &escalonador_curto;     //escalonador que escolhe o mais rápido

  self->m_so = (metricas_so){0, 0, 0, 0};

  init_mem(self);
  // coloca a CPU em modo usuário
  /*
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  cpue_muda_modo(self->cpue, usuario);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  */
  return self;
}

//################################################ FUNÇÕES DE DESTRUIÇÃO #######################################

void so_destroi(so_t *self)
{
  cpue_destroi(self->cpue);
  free(self);
}

void destroi_proc(proc_t* proc){
  cpue_destroi(proc->cpue_proc);
  mem_destroi(proc->mem_proc);
  free(proc);
}

//################################################ TRATAMENTO DE SISOPS #######################################

// chamada de sistema para leitura de E/S
// recebe em A a identificação do dispositivo
// retorna em X o valor lido
//            A o código de erro
static void so_trata_sisop_le(so_t *self)
{
  int disp = cpue_A(self->cpue);
  int val;
  err_t err;

  if (es_pronto(contr_es(self->contr), disp, leitura)){
    //esta pronto
    err = es_le(contr_es(self->contr), disp, &val);
    cpue_muda_A(self->cpue, err);
    
    if (err == ERR_OK) {
      cpue_muda_X(self->cpue, val);
      // incrementa o PC
      cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
    }
    cpue_muda_erro(self->cpue, ERR_OK, 0);

  } else {
    //nao esta pronto
    bloqueia_proc(self, (proc_bloq_t)leitura, disp);
    cpue_muda_erro(self->cpue, ERR_OCUP, 0);
  }
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// chamada de sistema para escrita de E/S
// recebe em A a identificação do dispositivo
//           X o valor a ser escrito
// retorna em A o código de erro
static void so_trata_sisop_escr(so_t *self)
{
  int disp = cpue_A(self->cpue);
  int val = cpue_X(self->cpue);
  err_t err;

  if (es_pronto(contr_es(self->contr), disp, escrita)){
    err = es_escreve(contr_es(self->contr), disp, val);
    cpue_muda_A(self->cpue, err);

    if (err == ERR_OK){
      cpue_muda_PC(self->cpue, cpue_PC(self->cpue)+2);
    }
    cpue_muda_erro(self->cpue, ERR_OK, 0);
  } else {
    bloqueia_proc(self, (proc_bloq_t)escrita, disp);
    cpue_muda_erro(self->cpue, ERR_OCUP, 0);
  }
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// chamada de sistema para término do processo
static void so_trata_sisop_fim(so_t *self)
{
  //metricas
  self->proc_exec->m_proc.tempo_retorno = rel_agora(contr_rel(self->contr)) - self->proc_exec->m_proc.existe_desde;
  salva_metricas_proc(self->proc_exec);

  proc_t* aux = self->proc_prt;
  if (self->proc_prt == self->proc_exec) { //o processo a ser destruido é o primeiro da lista de prontos
    self->proc_prt = self->proc_prt->prox;
    destroi_proc(self->proc_exec);

  } else {
    while( (aux->prox != NULL) && (aux->prox->id_proc != self->proc_exec->id_proc) ){
      aux = aux->prox;
    } //agora o aux aponta pro anterior ao atual

    aux->prox = self->proc_exec->prox;
    destroi_proc(self->proc_exec);
  }
  self->proc_exec = NULL;
  cpue_muda_erro(self->cpue, ERR_OK, 0);
}

// chamada de sistema para criação de processo
static void so_trata_sisop_cria(so_t *self)
{
  int id = cpue_A(self->cpue);
  proc_t* novo = novo_proc(self, id);

  //guarda o valor do rel no momento da criação
  novo->m_proc.existe_desde = rel_agora(contr_rel(self->contr));

  proc_t* aux = self->proc_prt;
  while(aux->prox != NULL){
    aux = aux->prox;
  }
  aux->prox = novo;   //coloca o novo processo no fim da fila de prontos

  if (id == 1) {
    init_mem_p1(self, novo->mem_proc);
  } else
  if (id == 2) {
    init_mem_p2(self, novo->mem_proc);
  } else {
    t_printf("Id inválido: %d", id);
    panico(self);
  }

  // incrementa o PC
  cpue_muda_PC(self->cpue, cpue_PC(self->cpue) + 2);
  cpue_muda_erro(self->cpue, ERR_OK, 0);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
}

// trata uma interrupção de chamada de sistema
static void so_trata_sisop(so_t *self)
{
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  // o tipo de chamada está no "complemento" do cpue
  so_chamada_t chamada = cpue_complemento(self->cpue);
  switch (chamada) {
    case SO_LE:
      so_trata_sisop_le(self);
      break;
    case SO_ESCR:
      so_trata_sisop_escr(self);
      break;
    case SO_FIM:
      so_trata_sisop_fim(self);
      break;
    case SO_CRIA:
      so_trata_sisop_cria(self);
      break;
    default:
      t_printf("so: chamada de sistema não reconhecida %d\n", chamada);
      panico(self);
  }
}

// trata uma interrupção de tempo do relógio
static void so_trata_tic(so_t *self)
{
  //debug
  t_printf("Interrupção de relógio");
  if ((cpue_modo(self->cpue) == zumbi) || (self->proc_exec == NULL)){   //se a cpu estiver no mode zumbi, verifica os processos e tenta troca
    verif_processos(self);
    troca_processo(self);
    return;
  }
  //não está no modo zumbi, há um processo executando
  self->proc_exec->tempo_exec += rel_periodo(contr_rel(self->contr));   //aumenta o tempo que ficou executando
  verif_processos(self);                                                //desbloqueia o que puder de processos

  if (self->proc_exec->prox == NULL){                                //só há um processo pronto
    return;
  } else if (self->proc_exec->tempo_exec >= self->quantum){         //se extrapolou o quantum
    //metricas
    self->proc_exec->m_proc.n_preemp++;

    //preempção
    bloqueia_proc(self, rel, -1);                                   //bloqueia o processo atual
    exec_copia_estado(contr_exec(self->contr), self->cpue);         //salva o contexto do proc
    if ( salva_contexto_proc(self, self->proc_exec) != ERR_OK){
      panico(self);
    }    
    troca_processo(self);                                           //fa z a troca
    return;
  }
}

// houve uma interrupção
void so_int(so_t *self, err_t err)
{
  self->m_so.n_int++;

  switch (err) {
    case ERR_SISOP:
      so_trata_sisop(self);     // ERR_OK se ta tudo bem, ERR_OCUP se deu ruim
      break;
    case ERR_TIC:
      so_trata_tic(self);
      return;
    default:
      t_printf("SO: interrupção não tratada [%s]", err_nome(err));
      self->paniquei = true;
  }

  if (cpue_erro(self->cpue) == ERR_OCUP){   //processo em exec bloqueou
    exec_copia_estado(contr_exec(self->contr), self->cpue);
    if (self->proc_exec != NULL){
      if ( salva_contexto_proc(self, self->proc_exec) != ERR_OK){
        panico(self);
      }
    }
    verif_processos(self);
    troca_processo(self); 
  }
  if (self->proc_exec == NULL) { //processo em exec acabou
    verif_processos(self);
    troca_processo(self);
  }
}

//################################################ FUNÇÕES DE CONTROLE #######################################
// retorna false se o sistema deve ser desligado
bool so_ok(so_t *self)
{
  return !self->paniquei;
}

static void panico(so_t *self) 
{
  t_printf("Problema irrecuperável no SO");
  self->paniquei = true;
  salva_metricas_so(self);
}

//################################################ FUNÇÕES DE INICIALIZAÇÃO DE MEM #######################################

static void init_mem_p1(so_t* self, mem_t* mem_p1){
  int p1[] = {
    #include "p1.maq"
  };
  int tam_p1 = sizeof(p1) / sizeof(p1[0]);

  for (int i = 0; i < tam_p1; i++) {
    if (mem_escreve(mem_p1, i, p1[i]) != ERR_OK) {
      t_printf("so.mem_p1: erro de memória, endereco %d\n", i);
      panico(self);
    }
  }

}

static void init_mem_p2(so_t* self, mem_t* mem_p2){
  int p2[] = {
    #include "p2.maq"
  };
  int tam_p2 = sizeof(p2) / sizeof(p2[0]);

  for (int i = 0; i < tam_p2; i++) {
    if (mem_escreve(mem_p2, i, p2[i]) != ERR_OK) {
      t_printf("so.mem_p2: erro de memória, endereco %d\n", i);
      panico(self);
    }
  }
}

// carrega um programa na memória
static void init_mem(so_t *self)
{
  // programa para executar na nossa CPU
  int init[] = {
  #include "init.maq"
  };
  int tam_init = sizeof(init)/sizeof(init[0]);

  // inicializa a memória com o programa
  mem_t *mem = contr_mem(self->contr);
  for (int i = 0; i < tam_init; i++) {
    if (mem_escreve(mem, i, init[i]) != ERR_OK) {
      t_printf("so.init_mem: erro de memória, endereco %d\n", i);
      panico(self);
    }
  }
}

//################################################ FUNÇÕES EXTRAS #######################################

static void salva_metricas_proc(proc_t* proc){
  FILE* file;

  if (proc->id_proc == 0){    //esse é o processo 0, reinicia o arquivo
    file = fopen("proc_metricas.txt", "w");
  } else {                    //não é o processo 0, abre o arquivo no modo 'append'
    file = fopen("proc_metricas.txt", "a");
  }

  if (file == NULL){
    t_printf("Não foi possível salvar dados do processo.");
    return;
  }
  char s[8][32] = {
    "Id do processo:       ",
    "Tempo de retorno:     ",
    "Tempo bloqueado:      ",
    "Tempo em execução:    ",
    "Tempo esperando:      ",
    "Número de bloqueios:  ",
    "Número de preempções: ",
    "Métricas coletadas do processo"
  };

  int d[7] = {
    proc->id_proc, 
    proc->m_proc.tempo_retorno, 
    proc->m_proc.tempo_bloq, 
    proc->m_proc.tempo_exec, 
    proc->m_proc.tempo_pronto, 
    proc->m_proc.n_bloqs, 
    proc->m_proc.n_preemp
  };

  fprintf(file, "%s\n%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n%s %d\n\n\n", s[7],
          s[0], d[0], 
          s[1], d[1],
          s[2], d[2],
          s[3], d[3],
          s[4], d[4],
          s[5], d[5],
          s[6], d[6]);
  fclose(file);
}

static void salva_metricas_so(so_t* so){
  FILE* file = fopen("so_metricas.txt", "w");

  if (file == NULL){
    t_printf("Não foi possível salvar dados do SO.");
    return;
  }
  char s[4][38] = {
    "Tempo total de sistema:  ",
    "Tempo total em execução: ",
    "Número de interrupções:  ",
    "Métricas do Sistema Operacional"
  };

  int d[7] = {
    so->m_so.tempo_total, 
    so->m_so.tempo_total_exec,
    so->m_so.n_int 
  };

  fprintf(file, "%s\n%s %d\n%s %d\n%s %d\n\n", s[3], 
          s[0], d[0], 
          s[1], d[1],
          s[2], d[2]);
  fclose(file);
}