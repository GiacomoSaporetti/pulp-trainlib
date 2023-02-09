/*
 * Copyright (C) 2021-2022 ETH Zurich and University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Authors: Davide Nadalini, Leonardo Ravaglia
*/ 

#include "pulp_train_utils_fp16.h"
#include "pulp_matmul_fp16.h"
#include "pulp_conv_pw_fp16.h"
#include "pulp_train_defines.h"


void pulp_conv_pw_fp16_fw_cl( void * PointWise_Conv_args_fp16 )
{
  struct PointWise_Conv_args_fp16 * PW_args = (struct PointWise_Conv_args_fp16 *) PointWise_Conv_args_fp16;
  struct matMul_args_fp16 matMul_args;

  int pW = PW_args->coeff->W;
  int pH = PW_args->coeff->H;
  fp16 *coeffData = PW_args->coeff->data;
  fp16 *outData = PW_args->output->data;
  fp16 *inData = PW_args->input->data;

  int W_in = PW_args->input->W;
  int H_in = PW_args->input->H;
  int Cin = PW_args->input->C;
  int Cout = PW_args->output->C;

  int opt_matmul_type = PW_args->opt_matmul_type_fw;
  fp16 * transp_buffer = PW_args->transp_buffer;

  #define OPT_PW_FW
  #ifndef OPT_PW_FW
  // NON-OPTIMIZED
  matMul_args.A = coeffData;
  matMul_args.B = inData;
  matMul_args.C = outData;
  matMul_args.N = Cout;
  matMul_args.M = H_in*W_in;
  matMul_args.K = pW*pH*Cin;
  matMul_args.trans_B = 0;

  #ifndef OPTIMIZE
  pi_cl_team_fork(NUM_CORES, mm_fp16, &matMul_args);
  #else
  struct mm_manager_args_fp16 man_args;
  man_args.mm_args = &matMul_args;
  man_args.layer_type = LAYER_PW_CONV;
  man_args.step_type = STEP_FW;
  man_args.matmul_type = opt_matmul_type; //MATMUL_TYPE;
  pi_cl_team_fork(NUM_CORES, mm_manager_fp16, &man_args);
  #endif

  #else

  // OPTIMIZED
  // Transpose the input
  struct transp_args_fp16 tr_args;
  tr_args.matrix = inData;
  tr_args.transp_matrix = transp_buffer;
  tr_args.N = Cin;
  tr_args.M = H_in*W_in;
  pi_cl_team_fork(NUM_CORES, transpose_fp16, &tr_args);
  // Perform forward
  matMul_args.A = coeffData;
  matMul_args.B = transp_buffer; //inData;
  matMul_args.C = outData;
  matMul_args.N = Cout;
  matMul_args.M = H_in*W_in;
  matMul_args.K = Cin;
  matMul_args.trans_B = 1;

  #ifndef OPTIMIZE
  pi_cl_team_fork(NUM_CORES, mm_fp16, &matMul_args);
  #else
  struct mm_manager_args_fp16 man_args;
  man_args.mm_args = &matMul_args;
  man_args.layer_type = LAYER_PW_CONV;
  man_args.step_type = STEP_FW;
  man_args.matmul_type = opt_matmul_type; //MATMUL_TYPE;
  pi_cl_team_fork(NUM_CORES, mm_manager_fp16, &man_args);
  #endif

  #endif

  #ifdef DEBUG
  printf("FORWARD PW LAYER \n\n");
  for (int i=0; i<Cout*output->W*output->H; i++) {
    if ((i+1)%output->W==0) {
      printf(" %f \n\n", i, outData[i]);
    }
    else
      printf(" %f \n", outData[i]);
  }
  printf("\n");
  #endif

  return;
}



void pulp_conv_pw_fp16_bw_cl( void * PointWise_Conv_args_fp16 )
{
  struct PointWise_Conv_args_fp16 * PW_args = (struct PointWise_Conv_args_fp16 *) PointWise_Conv_args_fp16;
  int skip_in_grad = PW_args->skip_in_grad;

  pulp_conv_pw_fp16_bw_param_grads_cl(PointWise_Conv_args_fp16); 
  if (skip_in_grad == 0)
  {
    pulp_conv_pw_fp16_bw_input_grads_cl(PointWise_Conv_args_fp16); 
  }
}



void pulp_conv_pw_fp16_bw_param_grads_cl( void * PointWise_Conv_args_fp16 )
{
  struct PointWise_Conv_args_fp16 * PW_args = (struct PointWise_Conv_args_fp16 *) PointWise_Conv_args_fp16;
  struct matMul_args_fp16 matMul_args;

  //input dimensions
  int W_in = PW_args->input->W;
  int H_in = PW_args->input->H;
  int C_in = PW_args->input->C;
  //kernel dimensions
  int pW = PW_args->coeff->W;
  int pH = PW_args->coeff->H;
  //output dimensions
  int W_out = PW_args->output->W;
  int H_out = PW_args->output->H;
  int C_out = PW_args->output->C;

  #ifdef DEBUG
  printf("OUTDIM %d %d %d ", W_in, H_in, C_in);
  #endif

  fp16 * inData = PW_args->input->data;
  fp16 * inDiff = PW_args->input->diff;

  fp16 * coeffData = PW_args->coeff->data;
  fp16 * coeffDiff = PW_args->coeff->diff;

  fp16 * outData = PW_args->output->data;
  fp16 * outDiff = PW_args->output->diff;

  int opt_matmul_type = PW_args->opt_matmul_type_wg;

  // COMPUTE GRADIENT
  matMul_args.A = outDiff;
  matMul_args.B = inData;  // transpose this
  matMul_args.C = coeffDiff;
  matMul_args.N = C_out;
  matMul_args.M = C_in;
  matMul_args.K = W_out*H_out;
  matMul_args.trans_B = 1;

  #ifndef OPTIMIZE
  pi_cl_team_fork(NUM_CORES, mm_fp16, &matMul_args);
  #else
  struct mm_manager_args_fp16 man_args;
  man_args.mm_args = &matMul_args;
  man_args.layer_type = LAYER_PW_CONV;
  man_args.step_type = STEP_WGT_GRAD;
  man_args.matmul_type = opt_matmul_type; //MATMUL_TYPE;
  pi_cl_team_fork(NUM_CORES, mm_manager_fp16, &man_args);
  #endif

  #ifdef DEBUG
  printf("%d %d %d %d\n\n", pW,pH,C_in,C_out);

  printf("GRADIENT PW LAYER \n\n");
  for (int i=0; i<pW*pH*C_out*C_in; i++) {
    if ((i+1)%C_out==0) {
      printf(" %f \n\n", i, coeffDiff[i]);
    }
    else
      printf(" %f \n", coeffDiff[i]);
  }
  printf("\n");
  #endif
}



void pulp_conv_pw_fp16_bw_input_grads_cl( void * PointWise_Conv_args_fp16 )
{
  struct PointWise_Conv_args_fp16 * PW_args = (struct PointWise_Conv_args_fp16 *) PointWise_Conv_args_fp16;
  struct matMul_args_fp16 matMul_args;

  //input dimensions
  int W_in = PW_args->input->W;
  int H_in = PW_args->input->H;
  int C_in = PW_args->input->C;
  //kernel dimensions
  int pW = PW_args->coeff->W;
  int pH = PW_args->coeff->H;
  //output dimensions
  int W_out = PW_args->output->W;
  int H_out = PW_args->output->H;
  int C_out = PW_args->output->C;

  #ifdef DEBUG
  printf("OUTDIM %d %d %d ", W_out, H_out, C_out);
  #endif

  fp16 * inData = PW_args->input->data;
  fp16 * inDiff = PW_args->input->diff;

  fp16 * coeffData = PW_args->coeff->data;
  fp16 * coeffDiff = PW_args->coeff->diff;

  fp16 * outData = PW_args->output->data;
  fp16 * outDiff = PW_args->output->diff;

  int opt_matmul_type = PW_args->opt_matmul_type_ig;
  fp16 * transp_buffer = PW_args->transp_buffer;

  //#define OPT_PW_IG
  #ifndef OPT_PW_IG

  // NON-OPTIMIZED
  matMul_args.A = coeffData; // transp ?
  matMul_args.B = outDiff;
  matMul_args.C = inDiff;
  matMul_args.N = C_in;
  matMul_args.M = W_in*H_in;
  matMul_args.K = pW*pH*C_out;
  matMul_args.trans_B = 0;
  
  #ifndef OPTIMIZE
  pi_cl_team_fork(NUM_CORES, mm_fp16, &matMul_args);
  #else
  struct mm_manager_args_fp16 man_args;
  man_args.mm_args = &matMul_args;
  man_args.layer_type = LAYER_PW_CONV;
  man_args.step_type = STEP_IN_GRAD;
  man_args.matmul_type = opt_matmul_type; //MATMUL_TYPE;
  pi_cl_team_fork(NUM_CORES, mm_manager_fp16, &man_args);
  #endif

  #else

  // OPTIMIZED
  struct transp_args_fp16 tr_args;
  // Transpose weights in the first part of the buffer
  tr_args.matrix = coeffData;
  tr_args.transp_matrix = transp_buffer;
  tr_args.N = C_in;
  tr_args.M = C_out;
  pi_cl_team_fork(NUM_CORES, transpose_fp16, &tr_args);
  tr_args.matrix = outDiff;
  tr_args.transp_matrix = &(*(transp_buffer + C_in*C_out));
  tr_args.N = C_out;
  tr_args.M = W_out*H_out;
  pi_cl_team_fork(NUM_CORES, transpose_fp16, &tr_args);
  // Compute input gradient
  matMul_args.A = transp_buffer; // coeffData; // transp ?
  matMul_args.B = &(*(transp_buffer + C_in*C_out)); // outDiff;
  matMul_args.C = inDiff;
  matMul_args.N = C_in;
  matMul_args.M = W_in*H_in;
  matMul_args.K = C_out;
  matMul_args.trans_B = 1;
  
  #ifndef OPTIMIZE
  pi_cl_team_fork(NUM_CORES, mm_fp16, &matMul_args);
  #else
  struct mm_manager_args_fp16 man_args;
  man_args.mm_args = &matMul_args;
  man_args.layer_type = LAYER_PW_CONV;
  man_args.step_type = STEP_IN_GRAD;
  man_args.matmul_type = opt_matmul_type; //MATMUL_TYPE;
  pi_cl_team_fork(NUM_CORES, mm_manager_fp16, &man_args);
  #endif

  #endif

  #ifdef DEBUG
  // to PRINT outDiff orderly
  printf("ERROR PROP PW LAYER \n\n");
  for (int i=0; i<W_in*H_in*C_in; i++) {
    if ((i+1)%W_in==0) {
      printf(" %f \n", i, inDiff[i]);
      if ((i+1)%(W_in*H_in)==0)
        printf("\n");
    }
    else
      printf(" %f ", i, inDiff[i]);
  }
  #endif
}
