/*
 Copyright (C) 2011 Melissa Gymrek <mgymrek@mit.edu>
*/

#include <iostream>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <fftw3.h>

#include "FFT_four_nuc_vectors.h"
#include "ISatellite.h"
#include "STRDetector.h"
#include "runtime_parameters.h"
#include "EntropyDetection.h"
#include "runtime_parameters.h"
#include "TukeyWindowGenerator.h"
#include "HammingWindowGenerator.h"
#include "common.h"
#include "runtime_parameters.h"

using namespace std;

int new_window_size = fft_window_size;
int new_window_step = fft_window_step;

STRDetector::STRDetector(){}


/*
   FFT-with-FFTW copied from http://nashruddin.com/fft-with-fftw-example.html
*/


bool STRDetector::ProcessRead(MSReadRecord* read) {
  // Preprocessing checks
  if (read->nucleotides.length() < (fft_window_size-1)) {
    if (why_not_debug) {
      cerr << "Read length must be greater than the window size" << endl;
    }
    return false;
  }

  if (calculate_N_percentage(read->nucleotides) > percent_N_discard) {
    if (why_not_debug) {
      cerr << "Discarding read '" << read->ID << "' (too many Ns)" << endl;
    }
    return false;
  }

  if (read->nucleotides.length() < min_read_length) {
    if (why_not_debug) {
      cerr << "Discarding read '" << read->ID << "' (read length too short)" << endl;
    }
    return false;
  }

  if (read->nucleotides.length() > max_read_length) {
    if (why_not_debug) {
      cerr << "Discarding read '" << read->ID << "' (read length too long)" << endl;
    }
    return false;
  }
  
  vector<double> window_lobe_detection;

  /*
    Step 1 -
    For each window (of fft_window_size & fft_window_step ):
    1. Calculate FFT on nucleotides matrix
    2. Sum up the matrix
    3. Detect Lobe on summed matrix
    4. Store results in window_lobes_detection vector
  */
  int nuc_start;
  int nuc_end;
  string detected_microsatellite_nucleotides;
  EntropyDetection ed_filter(read->nucleotides, fft_window_size, fft_window_step);
  if (!ed_filter.EntropyIsAboveThreshold()) {
    if (why_not_debug) {
      cerr << "Entropy threshold failed" << endl;
    }
    return false;
  }
  size_t start, end;
  bool rep_end = false; // is the end actually repetitive?
  ed_filter.FindStartEnd(start, end, &rep_end);
  nuc_start = start * new_window_step + extend_flank; // added extend each by window step size
  nuc_end = (end + 2) * new_window_step - extend_flank;
  if (nuc_start >= read->nucleotides.length() || nuc_start <= 0 || 
      nuc_end-nuc_start + 1 <= 0 || nuc_start >= nuc_end || nuc_end-nuc_start+1 >= read->nucleotides.size() ||
      nuc_end >= read->nucleotides.size()) 
    return false;
  // allow more nucleotides in detection step
  if (nuc_start-extend_flank >= 0 && nuc_end - extend_flank +1 <= read->nucleotides.size()) {
    detected_microsatellite_nucleotides = read->nucleotides.substr(nuc_start-extend_flank,nuc_end - nuc_start+1);      
  } else {
    detected_microsatellite_nucleotides = read->nucleotides.substr(nuc_start,nuc_end - nuc_start+1);     
  }
  /*
    Step 3 -
    Do FFT on the newly extract microsatellite region
  */
  TukeyWindowGenerator *pTukeyWindowGenerator = TukeyWindowGenerator::GetTukeyWindowSingleton();
  const WindowVector *pTukeyWindow = pTukeyWindowGenerator->GetWindow( detected_microsatellite_nucleotides.length() ) ;
  
  FFT_FOUR_NUC_VECTORS ms;
  ms.resize(1024);
  ms.build_plan(1024);
  ms.set_nucleotides(detected_microsatellite_nucleotides);
  ms.multiply_nuc_matrix_by_vector(*pTukeyWindow);
  ms.pad_zeros_to_end(detected_microsatellite_nucleotides.length());
  ms.execute();
  ms.out_complex_to_magnitude();
  ms.create_summed_matrix();  
  ms.destroy_plan();
  

  /*
    Step 4 -
    Period Detection
  */
  size_t best_period = 0;
  size_t next_best_period = 0;
  string ms_period_nuc;
  
  char over_abundtant_nuc = OneAbundantNucleotide(detected_microsatellite_nucleotides, 1);
  
  if (over_abundtant_nuc != 0) {
    best_period = 1 ;
    ms_period_nuc = over_abundtant_nuc;
  } else {
    
    std::vector<double> &fft_vec = ms.summed_matrix;
    
    min_period=2;
    size_t lobe_width = round( 1024.0 / ((double)detected_microsatellite_nucleotides.length()) );
    HammingWindowGenerator *pHammingWindowGenerator = HammingWindowGenerator::GetHammingWindowSingleton();
    const WindowVector *pHamming_Noise = pHammingWindowGenerator->GetWindow( lobe_width );
    double noise_y = 0;
    std::vector<double> noise;
    noise.resize(lobe_width);
    for (size_t i = 0 ; i<noise.size(); ++i) {
      srand(1);
      noise[i] = fft_vec [ rand()%1024 ] ; 
      noise_y += noise[i] *  pHamming_Noise->at(i);
    }
    std::vector<double> energy;
    
    const WindowVector *pHamming_roi = pHammingWindowGenerator->GetWindow( lobe_width * 2 + 1 );
    
    for (size_t period = min_period; period<=max_period_to_try; period++) {
      
      double period_energy = 0 ;
      
      size_t f = 1 ;
      double center = 1024 * (double)f / (double)period;
      
      double roi_y = 0 ;
      size_t roi_x_start = round(center - (double)lobe_width);
      size_t roi_x_end = round(center + (double)lobe_width+1) ;
      if (roi_x_end>1023)
	roi_x_end=1023;
      size_t roi_x_size = roi_x_end - roi_x_start ;
      for ( size_t roi_x = roi_x_start ; roi_x < roi_x_end ; roi_x++ ) {
	roi_y += fft_vec[roi_x] * pHamming_roi->at(roi_x-roi_x_start);
      }
      
      period_energy = (roi_y - noise_y) * period;//pow((roi_y - noise_y) * period, 2);
      energy.push_back(period_energy);
    }
    
    //Find the max. period
    std::vector<double>::iterator max_it = max_element(energy.begin(),energy.end());
    size_t max_index = distance(energy.begin(), max_it);
    
    best_period = max_index + min_period;
    float best_energy = energy.at(max_index);
    float next_best_energy = 0;
        
    for (size_t newperiod = min_period; newperiod<=max_period; newperiod++) {
      if (newperiod != best_period){
	if ((energy.at(newperiod - min_period))>next_best_energy){
	  next_best_energy = energy.at(newperiod - min_period);
	  next_best_period = newperiod;
	}
      }
    }

    // check that energy is high enough
    if (best_energy < period_energy_threshold) {
      if (why_not_debug) {
	cerr <<"Energy below threshold" << endl;
      }
      return false;
    }
    //    cout << best_energy << " " << next_best_energy  << " " << best_period << endl;
    
    if (fabs(next_best_energy-best_energy)/((best_energy+next_best_energy)/2)  > closeness) {
      next_best_period = 0;
    }
    if (next_best_period == 4 && best_period == 2) {
      best_period = 4;
      next_best_period = 0;
    }
  }
  
  
  /*
    Step 6 - Store values in the Read Record
  */
  read->ms_start = nuc_start;
  read->ms_end   = nuc_end;
  read->ms_repeat_best_period = best_period;
  read->ms_repeat_next_best_period = next_best_period;
  
  // set indices of left, STR, and right regions
  read->left_flank_nuc = (nuc_start>0) ? read->nucleotides.substr(0, read->ms_start) : "-" ;
  read->right_flank_nuc = read->nucleotides.substr(read->ms_end+1); // changed mgymrek
  read->detected_ms_region_nuc = detected_microsatellite_nucleotides;

  
  // adjust for max flank region lengths
  // if repetitive end, don't trim
  read->left_flank_index_from_start = 0;
  read->right_flank_index_from_end = 0;
  if (read->left_flank_nuc.size() > max_flank_len & !rep_end) {
    string left_flank = read->left_flank_nuc;
    read->left_flank_nuc = left_flank.substr(left_flank.length()-max_flank_len,
					     max_flank_len);
    read->left_flank_index_from_start = left_flank.length() - max_flank_len;
  }
  if (read->right_flank_nuc.size() > max_flank_len & !rep_end) {
    string right_flank = read->right_flank_nuc;
    read->right_flank_nuc = right_flank.substr(0, max_flank_len);
    read->right_flank_index_from_end = right_flank.length() - max_flank_len;
    }
  read->nucleotides = read->left_flank_nuc + read->detected_ms_region_nuc+read->right_flank_nuc;
  read->quality_scores = read->quality_scores.substr(read->left_flank_index_from_start,read->nucleotides.size());
  read->detected_ms_nuc = ms_period_nuc;

  if (why_not_debug ) {
    cerr << read->left_flank_nuc.length() << " " << read->right_flank_nuc.length() << " " << best_period << endl;
  }
  return ((read->left_flank_nuc.length() >= min_flank_len) & (read->right_flank_nuc.length() >= min_flank_len) &
	  best_period <= max_period & best_period >= min_period);
}

