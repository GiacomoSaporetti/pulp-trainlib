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
 * Authors: Davide Nadalini, Leonardo Ravaglia, Alberto Dequino
*/ 

#include "pulp_train_utils_fp32.h"
#include "pulp_im2col_fp32.h"

/**
 * @brief IM2COL with padding and stride
 * 
 * @param void_args 
 */
void pulp_im2col_fp32(void * void_args){

  // unpack args
  struct im2col_args * args = (struct im2col_args *)void_args;
  struct blob * input = args->input;
  struct blob * coeff = args->c;
  struct blob * output = args->output;

  float * i2c_buf = args->pBuffer;

  uint8_t Lpad = args->Lpad;
  uint8_t Rpad = args->Rpad;
  uint8_t Upad = args->Upad;
  uint8_t Dpad = args->Dpad;
  uint8_t mod = args->mod;
  uint8_t DW = args->DW;
  uint8_t Hstr = args->stride_h;
  uint8_t Wstr = args->stride_w;
  // Flag to activate the DMA version of the IM2COL
  uint8_t USE_DMA = args->USE_DMA;
  uint8_t HWC = args->HWC;

  // activations dimensions, w/o padding
  uint32_t Win = input->W;
  uint32_t Hin = input->H;
  uint32_t Cin = input->C;
  // kernel dimensions
  uint32_t Wk = coeff->W;
  uint32_t Hk = coeff->H;
  // input channels size
  uint32_t Wo = output->W;
  uint32_t Ho = output->H;
  uint32_t Co = output->C;

  // Set up internal variables (simpify external interface)
  Ho = Hin - Hk + 1;
  Wo = Win - Wk + 1;

  // Set up im2col variables for padding and stride
  uint32_t Htot, Wtot;
  Htot = (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
  Wtot = (Win-Wk+Lpad+Rpad+Wstr)/Wstr;

  #if NUM_CORES > 1
  // Definitions for parallelism
  int blockSize, start, stop;
  if (HWC == 0 && mod == 0) {
    blockSize = (Cin+NUM_CORES-1) / NUM_CORES;
    start = pi_core_id()*blockSize;
    stop = start+blockSize > Cin ? Cin : start+blockSize;
  }
  else if (HWC == 0 && mod == 1) {
    blockSize = (Co+NUM_CORES-1) / NUM_CORES;
    start = pi_core_id()*blockSize;
    stop = start+blockSize > Co ? Co : start+blockSize;
  }
  else if (HWC == 1 && mod == 0) {
    blockSize = (Htot+NUM_CORES-1) / NUM_CORES;
    start = pi_core_id()*blockSize;
    stop = start+blockSize > Htot ? Htot : start+blockSize;
  }
  else if (HWC == 1 && mod == 1) {
    blockSize = (Hin+NUM_CORES-1) / NUM_CORES;
    start = pi_core_id()*blockSize;
    stop = start+blockSize > Hin ? Hin : start+blockSize;
  }
  #else
  int start, stop; 
  if (HWC == 0 && mod == 0) {
    start = 0;
    stop = Cin;    
  }
  else if (HWC == 0 && mod == 1) {
    start = 0;
    stop = Co;
  }
  else if (HWC == 1 && mod == 0) {
    start = 0;
    stop = Htot;
  }
  else if (HWC == 1 && mod == 1) {
    start = 0;
    stop = Hin;
  }
  #endif

  /**
   * USE CHW FORMAT (ADJACENT ELEMENTS ARE ROW ELEMENTS OF THE INPUT OR OUTPUT MATRIX)
   */
  if (HWC == 0) {
    /**
     * LOCAL L1 IM2COL (FROM L1 TENSOR TO L1 IM2COL_BUFFER)
     */
    if (USE_DMA == 0) {
      // FORWARD & WEIGHT GRAD
      if (mod==0)
      {
        if ((Hin-Hk+Upad+Dpad+Hstr) % Hstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid H stride (non multiple H sizes): have H_in=%d, H_ker=%d, U_pad=%d, D_pad=%d, H_stride=%d, remainder=%d", Hin, Hk, Upad, Dpad, Hstr, (Hin-Hk+Upad+Dpad+Hstr) % Hstr); return;}
        else                                        Htot = (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        if ((Win-Wk+Lpad+Rpad+Wstr) % Wstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid W stride (non multiple W sizes): have W_in=%d, W_ker=%d, L_pad=%d, R_pad=%d, W_stride=%d, remainder=%d", Win, Wk, Lpad, Rpad, Wstr, (Win-Wk+Lpad+Rpad+Wstr) % Wstr); return;}
        else                                        Wtot = (Win-Wk+Lpad+Rpad+Wstr)/Wstr;

        int padding = Lpad + Rpad + Upad + Dpad;

        for (int ho=0; ho<Htot/*Ho+2*pad*/; ho++) {
          for (int wo=0; wo<Wtot/*Wo+2*pad*/; wo++) {
            for (int ci=start; ci<stop; ci++) {
              // IM2COL buffer coordinates
              int kernel_idx = ci*Hk*Wk;
              int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
              // Input tensor coordinates
              int receptive_field_idx = (wo*Wstr-Lpad) + (ho*Hstr-Upad)*Win + ci*Hin*Win;
              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  // IM2COl buffer coordinate update
                  int i2c_inner_idx = wk + hk*Wk;
                  // Input tensor coordinate update
                  int in_inner_idx = wk + hk*Win;
                  // Padding condition
                  int w_pad_cond = wk + wo*Wstr;
                  int h_pad_cond = hk + ho*Hstr;

                  if ((padding>0)&&((h_pad_cond<Upad) || (w_pad_cond<Lpad) || (h_pad_cond>Ho+(Hk)-Dpad) || (w_pad_cond>Wo+(Wk)-Rpad))) {
                    // Padding
                    i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = 0;
                    //printf("(pad) i2c_buf[%d]=%f                        kernel_idx=%d, segment_idx=%d, ho=%d\n", kernel_idx+segment_idx, i2c_buf[kernel_idx+segment_idx], kernel_idx, segment_idx, ho);
                  }
                  else {
                    // Fill IM2COL buffer
                    i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = input->data[receptive_field_idx+in_inner_idx];
                    //printf("(i2c) i2c_buf[%d]=%f (indata=%f)      kernel_idx=%d, segment_idx=%d, ho=%d\n", kernel_idx+segment_idx, i2c_buf[kernel_idx+segment_idx], input->data[receptive_field_idx], kernel_idx, segment_idx, ho);
                  }
                }
              }
            }
          }
        }
      }
      else // IN GRAD
      {
        // Set up variables for the in grad propagation
        //Ho = (int) (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        //Wo = (int) (Win-Wk+Rpad+Lpad+Wstr)/Wstr;

        int Hox = output->H;
        int Wox = output->W;
        
        for (int hi=0; hi<Hin; hi++) {
          for (int wi=0; wi<Win; wi++) {
            for (int co=start; co<stop; co++) {
              // IM2COL buffer coordinates
              int kernel_idx = co*Hk*Wk;
              int segment_idx = wi*Hk*Wk*Co + hi*Hk*Wk*Co*Win;
              // Output grad tensor coordinates
              int ho_rf = hi - (Hk-1);
              int wo_rf = wi - (Wk-1);
              int receptive_field_idx = wo_rf + ho_rf*Wox + co*Hox*Wox;

              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  // IM2COl buffer coordinates
                  int i2c_inner_idx = wk + hk*Wk;
                  // Output grad tensor coordinates
                  int out_inner_idx = wk + hk*Wox;
                  // Padding condition
                  int w_pad_cond = wk + wo_rf;
                  int h_pad_cond = hk + ho_rf;

                  if ((h_pad_cond<0) || (w_pad_cond<0) || (h_pad_cond>=Hox) || (w_pad_cond>=Wox)) {
                    // Padding
                    i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = 0;
                  }
                  else {
                    // Fill IM2COL buffer
                    i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = output->diff[receptive_field_idx+out_inner_idx];
                  }
                }
              }
            }
          }
        }
      }
    }

    /**
     * IM2COL FROM L2 DATA TO L1 IM2COL_BUFFER
     */
    else if (USE_DMA == 1) {
      // FORWARD & WEIGHT GRAD
      if (mod==0)
      {
        if ((Hin-Hk+Upad+Dpad+Hstr) % Hstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid H stride (non multiple H sizes): have H_in=%d, H_ker=%d, U_pad=%d, D_pad=%d, H_stride=%d, remainder=%d", Hin, Hk, Upad, Dpad, Hstr, (Hin-Hk+Upad+Dpad+Hstr) % Hstr); return;}
        else                                        Htot = (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        if ((Win-Wk+Lpad+Rpad+Wstr) % Wstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid W stride (non multiple W sizes): have W_in=%d, W_ker=%d, L_pad=%d, R_pad=%d, W_stride=%d, remainder=%d", Win, Wk, Lpad, Rpad, Wstr, (Win-Wk+Lpad+Rpad+Wstr) % Wstr); return;}
        else                                        Wtot = (Win-Wk+Lpad+Rpad+Wstr)/Wstr;     

        // Internal parallelization scheme on sizes
        int HblockSize = (Htot+NUM_CORES-1) / NUM_CORES;
        int Hstart = pi_core_id()*HblockSize;
        int Hstop = Hstart+HblockSize > Htot ? Htot : Hstart+HblockSize;      

        int padding = Lpad + Rpad + Upad + Dpad;

        if (padding == 0) {
          //for (int ho=0; ho<Htot; ho++) {
          for (int ho=Hstart; ho<Hstop; ho++) {
            for (int wo=0; wo<Wtot; wo++) {
              //for (int ci=start; ci<stop; ci++) {
              for (int ci=0; ci<Cin; ci++) {
                // IM2COl buffer coordinates
                int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
                int kernel_idx = ci*Hk*Wk;
                // Input tensor coordinates
                int receptive_field_idx = (wo*Wstr) + (ho*Hstr)*Win + ci*Hin*Win;

                // DMA Copy structures
                pi_cl_dma_copy_2d_t dma_i2cfw;

                // Load first data into L1A
                dma_i2cfw.dir = PI_CL_DMA_DIR_EXT2LOC;
                dma_i2cfw.merge = 0;
                dma_i2cfw.stride = 4*Win;
                dma_i2cfw.length = 4*Wk;
                dma_i2cfw.size = 4*Hk*Wk;
                dma_i2cfw.id = pi_core_id();
                dma_i2cfw.ext = (uint32_t) (input->data + receptive_field_idx);
                dma_i2cfw.loc = (uint32_t) &i2c_buf[segment_idx+kernel_idx];
                pi_cl_dma_memcpy_2d(&dma_i2cfw);  

                pi_cl_dma_wait(&dma_i2cfw);    
              }
            }
          }
        }
        else {
          for (int ho=0; ho<Htot; ho++) {
            for (int wo=0; wo<Wtot; wo++) {
              // Initialize padding conditions and variables
              int pad_l = Lpad - wo*Wstr;  
              int pad_r = wo*Wstr + (Wk) - Wtot - Rpad;
              int pad_u = Upad - ho*Hstr;
              int pad_d = ho*Hstr + (Hk) - Htot - Dpad;
              int row_size = Wk;                // Transfer lenght (length of a row)
              int col_size = Hk;
              int in_shift_idx = 0;             // Index to shift input reading
              int offs_l = 0, offs_u = 0;
              // Check if conditions for padding are met and assign zeros
              if (pad_l > 0)      {row_size -= pad_l;   in_shift_idx += pad_l;  offs_l = pad_l;}
              if (pad_r > 0)      {row_size -= pad_r;}
              if (pad_u > 0)      {col_size -= pad_u;   in_shift_idx += pad_u * Win;  offs_u = pad_u;}       
              if (pad_d > 0)      {col_size -= pad_d;}
              int transfer_size = row_size * col_size;

              //printf("ho=%d, wo=%d\tpad_l=%d, pad_r=%d, pad_u=%d, pad_d=%d\trow_size=%d, col_size=%d, transfer_size=%d\n", ho, wo, pad_l, pad_r, pad_u, pad_d, row_size, col_size, transfer_size);

              for (int ci=start; ci<stop; ci++) {
                // IM2COL buffer coordinates
                int kernel_idx = ci*Hk*Wk;
                int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
                // Input tensor coordinates
                int receptive_field_idx = (wo*Wstr-Lpad) + (ho*Hstr-Upad)*Win + ci*Hin*Win;

                // DMA Copy structures
                pi_cl_dma_copy_2d_t dma_i2cfw_pad;
                float load_buffer[transfer_size];
                float pad_buffer[Wk*Hk];

                // Load first data into L1A
                dma_i2cfw_pad.dir = PI_CL_DMA_DIR_EXT2LOC;
                dma_i2cfw_pad.merge = 0;
                dma_i2cfw_pad.stride = 4*Win;
                dma_i2cfw_pad.length = 4*row_size;
                dma_i2cfw_pad.size = 4*transfer_size;
                dma_i2cfw_pad.id = pi_core_id();
                dma_i2cfw_pad.ext = (uint32_t) (input->data + receptive_field_idx + in_shift_idx);
                dma_i2cfw_pad.loc = (uint32_t) load_buffer; 
                pi_cl_dma_memcpy_2d(&dma_i2cfw_pad);    

                // Initialize pad_buffer
                for (int i=0; i<Wk*Hk; i++) pad_buffer[i]=0;

                pi_cl_dma_wait(&dma_i2cfw_pad);    

                // Fill the pad_buffer
                for (int i=0; i<col_size; i++) { 
                  for (int j=0; j<row_size; j++) {
                    int pad_buffer_idx = offs_l + j + (offs_u+i)*Wk;
                    pad_buffer[pad_buffer_idx] = load_buffer[j+i*row_size];
                  }
                } 

                // Fill im2col
                for (int i=0; i<Wk*Hk; i++)   {i2c_buf[segment_idx+kernel_idx+i] = pad_buffer[i];}
              }
            }
          }
        }
      }
      else // IN GRAD
      {
        // Set up variables for the in grad propagation
        //Ho = (Hin-Hk+Upad+Dpad+Hstr);
        //Wo = (Win-Wk+Rpad+Lpad+Wstr);

        int Hox = output->H;
        int Wox = output->W;
        
        for (int hi=0; hi<Hin; hi++) {
          for (int wi=0; wi<Win; wi++) {
            for (int co=start; co<stop; co++) {
              // IM2COL buffer coordinates
              int kernel_idx = co*Hk*Wk;
              int segment_idx = wi*Hk*Wk*Co + hi*Hk*Wk*Co*Win;
              // Output grad tensor coordinates
              int ho_rf = hi - (Hk-1);
              int wo_rf = wi - (Wk-1);
              int receptive_field_idx = wo_rf + ho_rf*Wox + co*Hox*Wox;
              // Padding conditions
              int pad_l = -wo_rf;  int pad_r = wo_rf + (Wk-1);
              int pad_u = -ho_rf;  int pad_d = ho_rf + (Hk-1);
              int load_shift = 0;
              int offs_l = 0, offs_u = 0;
              // Transfer size
              int row_size = Wk;  int col_size = Hk;
              if (pad_l>0)          {row_size -= pad_l;   load_shift += pad_l;      offs_l = pad_l;}
              if (pad_r>=Wox)       {row_size -= pad_r-1;}
              if (pad_u>0)          {col_size -= pad_u;   load_shift += pad_u*Wox;  offs_u = pad_u;}
              if (pad_d>=Hox)       {col_size -= pad_d-1;}
              int transfer_size = col_size*row_size;
              //printf("hi=%d, wi=%d\tpad_l=%d, pad_r=%d, pad_u=%d, pad_d=%d\tcol_size=%d, row_size=%d, transfer_size=%d\toffs_l=%d, offs_r=%d\n", hi, wi, pad_l, pad_r, pad_u, pad_d, col_size, row_size, transfer_size, offs_l, offs_u);

              // DMA variables
              pi_cl_dma_copy_2d_t dma_i2cbw;
              float load_buffer[transfer_size];
              float pad_buffer[Hk*Wk];

              // Load first data into L1A
              dma_i2cbw.dir = PI_CL_DMA_DIR_EXT2LOC;
              dma_i2cbw.merge = 0;
              dma_i2cbw.stride = 4*Wox;
              dma_i2cbw.length = 4*row_size;
              dma_i2cbw.size = 4*transfer_size;
              dma_i2cbw.id = pi_core_id();
              dma_i2cbw.ext = (uint32_t) (output->diff + receptive_field_idx + load_shift);
              dma_i2cbw.loc = (uint32_t) load_buffer; 
              pi_cl_dma_memcpy_2d(&dma_i2cbw);    

              // Prepare pad_buffer 
              for (int idx=0; idx<Hk*Wk; idx++)   pad_buffer[idx] = 0;

              pi_cl_dma_wait(&dma_i2cbw);    

              // Fill pad_buffer
              for (int kh=0; kh<col_size; kh++) {
                for (int kw=0; kw<row_size; kw++) {
                  int pad_buf_idx = (kw+offs_l) + (kh+offs_u)*Wk;
                  pad_buffer[pad_buf_idx] = load_buffer[kw+kh*row_size];
                  //printf("pad_buffer[%d] = load_buffer[%d] = %f\n", pad_buf_idx, kw+kh*row_size, load_buffer[kw+kh*row_size]);
                }
              }

              // Fill im2col_buffer
              for (int idx=0; idx<Hk*Wk; idx++)   {
                i2c_buf[kernel_idx+segment_idx+idx] = pad_buffer[idx];
                //printf("pad_buffer[%d] = %f\n", idx, pad_buffer[idx]); 
              }
            }
          }
        }


      }    
    }

    // ERROR SIGNAL
    else {
      printf("\n[pulp_im2col_fp32: 414] Invalid USE_DMA parameter (not 0 or 1)\n");
    }
  }

  /**
   * USE HWC FORMAT (ADJACENT ELEMENTS ARE CHANNEL ELEMENTS IN THE INPUT OR OUTPUT MATRIX)
   */
  else if (HWC == 1) {
    /**
     * LOCAL L1 IM2COL (FROM L1 TENSOR TO L1 IM2COL_BUFFER)
     */
    if (USE_DMA == 0) {
      // FORWARD & WEIGHT GRAD
      if (mod==0)
      {
        if ((Hin-Hk+Upad+Dpad+Hstr) % Hstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid H stride (non multiple H sizes): have H_in=%d, H_ker=%d, U_pad=%d, D_pad=%d, H_stride=%d, remainder=%d", Hin, Hk, Upad, Dpad, Hstr, (Hin-Hk+Upad+Dpad+Hstr) % Hstr); return;}
        else                                        Htot = (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        if ((Win-Wk+Lpad+Rpad+Wstr) % Wstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid W stride (non multiple W sizes): have W_in=%d, W_ker=%d, L_pad=%d, R_pad=%d, W_stride=%d, remainder=%d", Win, Wk, Lpad, Rpad, Wstr, (Win-Wk+Lpad+Rpad+Wstr) % Wstr); return;}
        else                                        Wtot = (Win-Wk+Lpad+Rpad+Wstr)/Wstr;

        int padding = Lpad + Rpad + Upad + Dpad;

        if (padding == 0) {

          for (int ho=start; ho<stop/*Htot*/; ho++) {
            for (int wo=0; wo<Wtot/*Wtot*/; wo++) {
              // Im2Col indices
              int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
              // Input activation indices
              int input_idx = (wo*Wstr-Lpad)*Cin + (ho*Hstr-Upad)*Cin*Win;
              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  for (int ci=0; ci<Cin; ci++) {
                    // Im2Col indices
                    int i2c_inner_idx = ci + wk*Cin + hk*Cin*Wk;
                    // Input activation indices                    
                    int act_idx = ci + wk*Cin + hk*Cin*Win;
                    // Fill im2col buffer
                    i2c_buf[segment_idx+i2c_inner_idx] = input->data[input_idx+act_idx];
                  }
                }
              }
            }
          }

        }
        else {

          for (int ho=start; ho<stop/*Htot*/; ho++) {
            for (int wo=0; wo<Wtot/*Wtot*/; wo++) {
              // Im2Col indices
              int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
              // Input activation indices
              int input_idx = (wo*Wstr-Lpad)*Cin + (ho*Hstr-Upad)*Cin*Win;
              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  for (int ci=0; ci<Cin; ci++) {
                    // Im2Col indices
                    int i2c_inner_idx = ci + wk*Cin + hk*Cin*Wk;
                    // Input activation indices                    
                    int act_idx = ci + wk*Cin + hk*Cin*Win;
                    // Fill im2col buffer
                    i2c_buf[segment_idx+i2c_inner_idx] = input->data[input_idx+act_idx];
                  }
                }
              }
            }
          }

        }
      }
      else // IN GRAD
      {
        // Set up variables for the in grad propagation
        //Ho = (int) (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        //Wo = (int) (Win-Wk+Rpad+Lpad+Wstr)/Wstr;

        int Hox = output->H;
        int Wox = output->W;
        
        printf("[pulp_im2col_fp32:] HWC Im2Col for IN GRAD not implemented!!");

        for (int hi=0; hi<Hin; hi++) {
          for (int wi=0; wi<Win; wi++) {
            for (int hk=0; hk<Hk; hk++) {
              for (int wk=0; wk<Wk; wk++) {
                for (int co=0; co<Co; co++) {
                  // // IM2COL buffer coordinates
                  // int kernel_idx = co*Hk*Wk;
                  // int segment_idx = wi*Hk*Wk*Co + hi*Hk*Wk*Co*Win;
                  // // Output grad tensor coordinates
                  // int ho_rf = hi - (Hk-1);
                  // int wo_rf = wi - (Wk-1);
                  // int receptive_field_idx = wo_rf + ho_rf*Wox + co*Hox*Wox;

                  // // IM2COl buffer coordinates
                  // int i2c_inner_idx = wk + hk*Wk;
                  // // Output grad tensor coordinates
                  // int out_inner_idx = wk + hk*Wox;
                  // // Padding condition
                  // int w_pad_cond = wk + wo_rf;
                  // int h_pad_cond = hk + ho_rf;

                  // if ((h_pad_cond<0) || (w_pad_cond<0) || (h_pad_cond>=Hox) || (w_pad_cond>=Wox)) {
                  //   // Padding
                  //   i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = 0;
                  // }
                  // else {
                  //   // Fill IM2COL buffer
                  //   i2c_buf[kernel_idx+segment_idx+i2c_inner_idx] = output->diff[receptive_field_idx+out_inner_idx];
                  // }                                             
                }
              }
            }
          }
        }
      }
    }

    /**
     * IM2COL FROM L2 DATA TO L1 IM2COL_BUFFER
     */
    if (USE_DMA == 1) {
      // FORWARD & WEIGHT GRAD
      if (mod==0)
      {
        if ((Hin-Hk+Upad+Dpad+Hstr) % Hstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid H stride (non multiple H sizes): have H_in=%d, H_ker=%d, U_pad=%d, D_pad=%d, H_stride=%d, remainder=%d", Hin, Hk, Upad, Dpad, Hstr, (Hin-Hk+Upad+Dpad+Hstr) % Hstr); return;}
        else                                        Htot = (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        if ((Win-Wk+Lpad+Rpad+Wstr) % Wstr > 0)     {printf("\n[pulp_im2col_fp32: 243] Invalid W stride (non multiple W sizes): have W_in=%d, W_ker=%d, L_pad=%d, R_pad=%d, W_stride=%d, remainder=%d", Win, Wk, Lpad, Rpad, Wstr, (Win-Wk+Lpad+Rpad+Wstr) % Wstr); return;}
        else                                        Wtot = (Win-Wk+Lpad+Rpad+Wstr)/Wstr;

        int padding = Lpad + Rpad + Upad + Dpad;

        if (padding == 0) {

          for (int ho=start; ho<stop/*Htot*/; ho++) {
            for (int wo=0; wo<Wtot/*Wtot*/; wo++) {
              // Im2Col indices
              int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
              // Input activation indices
              int input_idx = (wo*Wstr-Lpad)*Cin + (ho*Hstr-Upad)*Cin*Win;
              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  for (int ci=0; ci<Cin; ci++) {
                    // Im2Col indices
                    int i2c_inner_idx = ci + wk*Cin + hk*Cin*Wk;
                    // Input activation indices                    
                    int act_idx = ci + wk*Cin + hk*Cin*Win;
                    // Fill im2col buffer
                    i2c_buf[segment_idx+i2c_inner_idx] = input->data[input_idx+act_idx];
                  }
                }
              }
            }
          }

        }
        else {

          for (int ho=start; ho<stop/*Htot*/; ho++) {
            for (int wo=0; wo<Wtot/*Wtot*/; wo++) {
              // Im2Col indices
              int segment_idx = wo*Hk*Wk*Cin + ho*Hk*Wk*Cin*(Wtot);
              // Input activation indices
              int input_idx = (wo*Wstr-Lpad)*Cin + (ho*Hstr-Upad)*Cin*Win;
              for (int hk=0; hk<Hk; hk++) {
                for (int wk=0; wk<Wk; wk++) {
                  for (int ci=0; ci<Cin; ci++) {
                    // Im2Col indices
                    int i2c_inner_idx = ci + wk*Cin + hk*Cin*Wk;
                    // Input activation indices                    
                    int act_idx = ci + wk*Cin + hk*Cin*Win;
                    // Fill im2col buffer
                    i2c_buf[segment_idx+i2c_inner_idx] = input->data[input_idx+act_idx];
                  }
                }
              }
            }
          }
                    
        }
      }
      else // IN GRAD
      {
        // Set up variables for the in grad propagation
        //Ho = (int) (Hin-Hk+Upad+Dpad+Hstr)/Hstr;
        //Wo = (int) (Win-Wk+Rpad+Lpad+Wstr)/Wstr;

        int Hox = output->H;
        int Wox = output->W;
        
        printf("[pulp_im2col_fp32:] HWC Im2Col for IN GRAD not implemented!!");

        for (int hi=start; hi<stop; hi++) {
          for (int wi=0; wi<Win; wi++) {
            for (int hk=0; hk<Hk; hk++) {
              for (int wk=0; wk<Wk; wk++) {
                for (int co=0; co<Co; co++) {
                                                        
                }
              }
            }
          }
        }
      }
    }

    // ERROR SIGNAL
    else {
      printf("\n[pulp_im2col_fp32: 414] Invalid USE_DMA parameter (not 0 or 1)\n");
    }
    
  }

  // ERROR SIGNAL
  else {
    printf("[pulp_im2col_fp32:] Invalid HWC parameter (not 0 or 1)\n");
  }
}



void pulp_blocktransp_fp32 (void * void_args)
{
  struct blocktransp_args * args = (struct blocktransp_args *)void_args;
  float * weights = args->weights;
  float * bt_weights = args->bt_weights;
  uint32_t Cin = args->Cin;
  uint32_t Cout = args->Cout;
  uint32_t Hk = args->Hk;
  uint32_t Wk = args->Wk;

  uint32_t HW = Hk*Wk;

  uint32_t blockSize = (Cout+NUM_CORES-1) / NUM_CORES;
  uint32_t start = pi_core_id()*blockSize;
  uint32_t stop = start+blockSize > Cout ? Cout : start+blockSize;

  // Block tranposition
  // for (uint32_t k=0; k<Cout; k++)
  for (uint32_t k=start; k<stop; k++)
  {
    for (uint32_t c=0; c<Cin; c++)
    {
      for (uint32_t i=0; i<Hk*Wk; i++)
      {
        // OLD 
        //temp[i+k*HW+c*Cout*HW] = weights[i+c*HW+k*Cin*HW];
        //temp[i+k*HW+c*Cout*HW] = weights[(HW-1-i)+c*HW+k*Cin*HW];

        // OTHER MATRIX
        //bt_weights[i+k*HW+c*Cout*HW] = weights[i+c*HW+k*Cin*HW];
        bt_weights[i+k*HW+c*Cout*HW] = weights[(HW-1-i)+c*HW+k*Cin*HW];
      }
    }
  } 
}

