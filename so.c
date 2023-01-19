#include "so.h"
#include "tela.h"
#include <stdlib.h>

struct so_t {
  contr_t *contr;       // o controlador do hardware
  bool paniquei;        // apareceu alguma situação intratável
  cpu_estado_t *cpue;   // cópia do estado da CPU

  proc_t* proc_exec;   // ponteiro pro processo atual
  proc_t* proc_bloq;    // inicio da fila de processos bloqueados
  proc_t* proc_prt;     // inicio da fila de processos prontos
};

struct proc
{
  int id_proc;             // id do processo
  cpu_estado_t *cpue_proc; // estado da cpu do processo
  proc_estado_t est_proc;  // estado do processo
  mem_t *mem_proc;         // memoria do processo

  proc_bloq_t motivo_bloq;    // pq bloqueou
  int disp_bloq;              // qual disp bloqueou
  
  proc_t* prox;
};

// funções auxiliares
static void init_mem(so_t *self);
static void init_mem_p1(so_t *self, mem_t* mem_p1);
static void init_mem_p2(so_t *self, mem_t* mem_p2);
static void panico(so_t *self);

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

int escalonador(so_t *self, proc_t** novo)
{
  if (self->proc_prt == NULL){      //todos os processos estão bloqueados... ou
    if (self->proc_bloq == NULL){   //acabaram os processos
      return -1;
    }  
    return 0;
  }
  else {
    *novo = self->proc_prt;
    return 1;
  }
}

void troca_processo(so_t* self){
  proc_t* novo = NULL;
  int esc = escalonador(self, &novo);

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
    t_printf("Fim da execução.");
    panico(self);
    return;

  } else if (esc == 0) { //todos bloqueados
    self->proc_exec = NULL;
    cpue_muda_modo(self->cpue, zumbi);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);
    return;

  } else if (esc == 1) { //achou processo
    self->proc_exec = novo;
    restaura_contexto_proc(self, self->proc_exec);
    cpue_muda_modo(self->cpue, usuario);
    cpue_muda_erro(self->cpue, ERR_OK, 0);
    exec_altera_estado(contr_exec(self->contr), self->cpue);
    return;
  }
}

void desbloqueia_proc(so_t* self, proc_t* proc){
  t_printf("Desbloqueando proc %d", proc->id_proc);

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

proc_t* novo_proc(so_t* self, int id)
{
  proc_t* novo = malloc(sizeof(*novo));
  if (novo == NULL) return NULL;
  
  novo->cpue_proc = cpue_cria();
  novo->mem_proc = mem_cria(MEM_TAM);
  novo->id_proc = id;
  novo->est_proc = PRONTO;
  novo->prox = NULL;

  return novo;
}

void bloqueia_proc(so_t* self, proc_bloq_t motivo, int disp){
  t_printf("Bloqueando proc %d", self->proc_exec->id_proc);
  self->proc_exec->est_proc = BLOQ;
  self->proc_exec->motivo_bloq = motivo;
  self->proc_exec->disp_bloq = disp;

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

  init_mem(self);
  // coloca a CPU em modo usuário
  /*
  exec_copia_estado(contr_exec(self->contr), self->cpue);
  cpue_muda_modo(self->cpue, usuario);
  exec_altera_estado(contr_exec(self->contr), self->cpue);
  */
  return self;
}

void so_destroi(so_t *self)
{
  cpue_destroi(self->cpue);
  free(self);
}

// trata chamadas de sistema

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
  proc_t* aux = self->proc_prt;

  if (self->proc_prt == self->proc_exec){ //o processo a ser destruido é o primeiro da lista de prontos
    self->proc_prt = self->proc_prt->prox;

    cpue_destroi(self->proc_exec->cpue_proc);
    mem_destroi(self->proc_exec->mem_proc);
    free(self->proc_exec);
  
  } else {
    while( (aux->prox != NULL) && (aux->prox->id_proc != self->proc_exec->id_proc) ){
      aux = aux->prox;
    } //agora o aux aponta pro anterior ao atual
    aux->prox = self->proc_exec->prox;

    cpue_destroi(self->proc_exec->cpue_proc);
    mem_destroi(self->proc_exec->mem_proc);
    free(self->proc_exec);
  }
  self->proc_exec = NULL;
  cpue_muda_erro(self->cpue, ERR_OK, 0);
}

// chamada de sistema para criação de processo
static void so_trata_sisop_cria(so_t *self)
{
  int id = cpue_A(self->cpue);
  proc_t* novo = novo_proc(self, id);

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
  //t_printf("Interrupção de relógio");
  if (cpue_modo(self->cpue) == zumbi){
    verif_processos(self);
    troca_processo(self);
  }
/*
  if (self->proc_ini->prox == NULL){ //o processo é o único
    return;
  }

  if (self->proc_exec != NULL){
    self->proc_exec->est_proc = BLOQ; //força a troca de processo
    self->proc_exec->motivo_bloq = relogio;
    self->proc_exec->disp_bloq = -1;
  }
  troca_processo(self); //o contexto é restaurado aqui
*/
  return;
}

// houve uma interrupção
void so_int(so_t *self, err_t err)
{
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

// retorna false se o sistema deve ser desligado
bool so_ok(so_t *self)
{
  return !self->paniquei;
}

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
  
static void panico(so_t *self) 
{
  t_printf("Problema irrecuperável no SO");
  self->paniquei = true;
}
