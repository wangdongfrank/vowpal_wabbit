/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <fstream>
#include <sstream>
#include <float.h>
#ifdef _WIN32
#include <WinSock2.h>
#else
#include <netdb.h>
#endif
#include <string.h>
#include <stdio.h>
#include <assert.h>

#if defined(__SSE2__) && !defined(VW_LDA_NO_SSE)
#include <xmmintrin.h>
#endif

#include "gd.h"
#include "simple_label.h"
#include "accumulate.h"
#include "reductions.h"

using namespace std;

using namespace LEARNER;

namespace GD
{
  struct gd{
    bool active;
    bool active_simulation;
    float normalized_sum_norm_x;
    size_t no_win_counter;
    size_t early_stop_thres;
    float initial_constant;
    void (*predict)(gd&, learner&, example&);

    vw* all;
  };

  void sync_weights(vw& all);

  struct power_data {
    float minus_power_t;
    float minus_power_t_norm;
  };
  
  struct train_data {
    float update;
    power_data pt;
  };


float InvSqrt(float x){
  float xhalf = 0.5f * x;
  int i = *(int*)&x; // store floating-point bits in integer
  i = 0x5f3759d5 - (i >> 1); // initial guess for Newton's method
  x = *(float*)&i; // convert new bits into float
  x = x*(1.5f - xhalf*x*x); // One round of Newton's method
  return x;
}
  
  template<bool sqrt_rate, size_t adaptive, size_t normalized>
  inline float compute_rate_decay(power_data& s, float& fw)
  {
    weight* w = &fw;
    float rate_decay = 1.f;
    if(adaptive) {
      if (sqrt_rate)
	{  
#if defined(__SSE2__) && !defined(VW_LDA_NO_SSE)
	  __m128 eta = _mm_load_ss(&w[adaptive]);
	  eta = _mm_rsqrt_ss(eta);
	  _mm_store_ss(&rate_decay, eta);
#else
	  rate_decay = InvSqrt(w[adaptive]);
#endif
	}
      else
	rate_decay = powf(w[adaptive],s.minus_power_t);
    }
    if(normalized) {
      if (sqrt_rate)
	{
	  float inv_norm = 1.f / w[normalized];
	  if (adaptive)
	    rate_decay *= inv_norm;
	  else
	    rate_decay *= inv_norm*inv_norm;
	}
      else{
	float norm = w[normalized];
	rate_decay *= powf(norm*norm,s.minus_power_t_norm);
      }
    }
    return rate_decay;
  }

  template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
  inline void update_feature(train_data& s, float x, float& fw)
  {
    weight* w = &fw;
    if(!feature_mask || w[feature_mask]==1.){
      float rate_decay = compute_rate_decay<sqrt_rate, adaptive, normalized>(s.pt, fw);
      w[0] += s.update * rate_decay * x;
    }
  }
  
  template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
  void train(vw& all, example& ec, float update)
  {
    if (fabsf(update) == 0.f)
      return;
    
    float total_weight = ec.example_t;

    if(!all.holdout_set_off)
      total_weight -= (float)all.sd->weighted_holdout_examples; //exclude weights from test_only examples   
    
    float avg_norm = all.normalized_sum_norm_x / total_weight;
    if (sqrt_rate) avg_norm = sqrt(avg_norm);

    float minus_power_t_norm = (adaptive ? all.power_t : 0.f) -1.f;

    if (normalized) {
      if (sqrt_rate) 
	if (adaptive)
	  update /= avg_norm;
	else
	  update /= (avg_norm * avg_norm);
      else 
	update *= powf(avg_norm * avg_norm, minus_power_t_norm);
    }

    train_data d = {update, {-all.power_t, minus_power_t_norm}};
    
    foreach_feature<train_data,update_feature<sqrt_rate, adaptive, normalized, feature_mask> >(all, ec, d);
  }

  void end_pass(gd& g)
  {
    vw* all = g.all;
    
    sync_weights(*all);
    if(all->span_server != "") {
      if(all->adaptive)
	accumulate_weighted_avg(*all, all->span_server, all->reg);
      else 
        accumulate_avg(*all, all->span_server, all->reg, 0);	      
    }
    
    all->eta *= all->eta_decay_rate;
    if (all->save_per_pass)
      save_predictor(*all, all->final_regressor_name, all->current_pass);   
    
    all->current_pass++;
    
    if(!all->holdout_set_off)
      {
        if(summarize_holdout_set(*all, g.no_win_counter))
          finalize_regressor(*all, all->final_regressor_name);
        if((g.early_stop_thres == g.no_win_counter) &&
           ((all->check_holdout_every_n_passes <= 1) ||
            ((all->current_pass % all->check_holdout_every_n_passes) == 0)))
	  set_done(*all);
      }   
  }

struct string_value {
  float v;
  string s;
  friend bool operator<(const string_value& first, const string_value& second);
};

 inline float sign(float w){ if (w < 0.) return -1.; else  return 1.;}
 
 inline float trunc_weight(const float w, const float gravity){
   return (gravity < fabsf(w)) ? w - sign(w) * gravity : 0.f;
 }

bool operator<(const string_value& first, const string_value& second)
{
  return fabs(first.v) > fabs(second.v);
}

#include <algorithm>

  void audit_feature(vw& all, feature* f, audit_data* a, vector<string_value>& results, string prepend, string& ns_pre, size_t offset = 0, float mult = 1)
{ 
  ostringstream tempstream;
  size_t index = (f->weight_index + offset) & all.reg.weight_mask;
  weight* weights = all.reg.weight_vector;
  size_t stride_shift = all.reg.stride_shift;
  
  if(all.audit) tempstream << prepend;
  
  string tmp = "";
  
  if (a != NULL){
    tmp += a->space;
    tmp += '^';
    tmp += a->feature; 
  }
 
  if (a != NULL && all.audit){
    tempstream << tmp << ':';
  }
  else 	if ( index == ((( (constant << stride_shift) * all.wpp + offset)&all.reg.weight_mask)) && all.audit){
    tempstream << "Constant:";
  }  
  if(all.audit){
    tempstream << ((index >> stride_shift) & all.parse_mask) << ':' << mult*f->x;
    tempstream  << ':' << trunc_weight(weights[index], (float)all.sd->gravity) * (float)all.sd->contraction;
  }
  if(all.current_pass == 0 && all.inv_hash_regressor_name != ""){ //for invert_hash
    if ( index == (((constant << stride_shift) + offset )& all.reg.weight_mask))
      tmp = "Constant";

    ostringstream convert;
    convert << ((index >>stride_shift) & all.parse_mask);
    tmp = ns_pre + tmp + ":"+ convert.str();
    
    if(!all.name_index_map.count(tmp)){
      all.name_index_map.insert(std::map< std::string, size_t>::value_type(tmp, ((index >> stride_shift) & all.parse_mask)));
    }
  }

  if(all.adaptive && all.audit)
    tempstream << '@' << weights[index+1];
  string_value sv = {weights[index]*f->x, tempstream.str()};
  results.push_back(sv);
}

  void audit_features(vw& all, v_array<feature>& fs, v_array<audit_data>& as, vector<string_value>& results, string prepend, string& ns_pre, size_t offset = 0, float mult = 1)
{
  for (size_t j = 0; j< fs.size(); j++)
    if (as.begin != as.end)
      audit_feature(all, & fs[j], & as[j], results, prepend, ns_pre, offset, mult);
    else
      audit_feature(all, & fs[j], NULL, results, prepend, ns_pre, offset, mult);
}

void audit_quad(vw& all, feature& left_feature, audit_data* left_audit, v_array<feature> &right_features, v_array<audit_data> &audit_right, vector<string_value>& results, string& ns_pre, uint32_t offset = 0)
{
  size_t halfhash = quadratic_constant * (left_feature.weight_index + offset);

  ostringstream tempstream;
  if (audit_right.size() != 0 && left_audit && all.audit)
    tempstream << left_audit->space << '^' << left_audit->feature << '^';
  string prepend = tempstream.str();

  if(all.current_pass == 0 && audit_right.size() != 0 && left_audit)//for invert_hash
  {
    ns_pre = left_audit->space; 
    ns_pre = ns_pre + '^' + left_audit->feature + '^';
  }
 
  audit_features(all, right_features, audit_right, results, prepend, ns_pre, halfhash + offset, left_audit->x);
}

void audit_triple(vw& all, feature& f0, audit_data* f0_audit, feature& f1, audit_data* f1_audit, 
		  v_array<feature> &right_features, v_array<audit_data> &audit_right, vector<string_value>& results, string& ns_pre, uint32_t offset = 0)
{
  size_t halfhash = cubic_constant2 * (cubic_constant * (f0.weight_index + offset) + f1.weight_index + offset);

  ostringstream tempstream;
  if (audit_right.size() > 0 && f0_audit && f1_audit && all.audit)
    tempstream << f0_audit->space << '^' << f0_audit->feature << '^' 
	       << f1_audit->space << '^' << f1_audit->feature << '^';
  string prepend = tempstream.str();

  if(all.current_pass == 0 && audit_right.size() != 0 && f0_audit && f1_audit)//for invert_hash
  {
    ns_pre = f0_audit->space;
    ns_pre = ns_pre + '^' + f0_audit->feature + '^' + f1_audit->space + '^' + f1_audit->feature + '^';
  }
  audit_features(all, right_features, audit_right, results, prepend, ns_pre, halfhash + offset);  
}

void print_features(vw& all, example& ec)
{
  weight* weights = all.reg.weight_vector;
  
  if (all.lda > 0)
    {
      size_t count = 0;
      for (unsigned char* i = ec.indices.begin; i != ec.indices.end; i++)
	count += ec.audit_features[*i].size() + ec.atomics[*i].size();
      for (unsigned char* i = ec.indices.begin; i != ec.indices.end; i++) 
	for (audit_data *f = ec.audit_features[*i].begin; f != ec.audit_features[*i].end; f++)
	  {
	    cout << '\t' << f->space << '^' << f->feature << ':' << ((f->weight_index >> all.reg.stride_shift) & all.parse_mask) << ':' << f->x;
	    for (size_t k = 0; k < all.lda; k++)
	      cout << ':' << weights[(f->weight_index+k) & all.reg.weight_mask];
	  }
      cout << " total of " << count << " features." << endl;
    }
  else
    {
      vector<string_value> features;
      string empty;
      string ns_pre;
      
      for (unsigned char* i = ec.indices.begin; i != ec.indices.end; i++){ 
        ns_pre = "";
	audit_features(all, ec.atomics[*i], ec.audit_features[*i], features, empty, ns_pre, ec.ft_offset);
        ns_pre = "";
      }
      for (vector<string>::iterator i = all.pairs.begin(); i != all.pairs.end();i++) 
	{
	  int fst = (*i)[0];
	  int snd = (*i)[1];
	  for (size_t j = 0; j < ec.atomics[fst].size(); j++)
	    {
	      audit_data* a = NULL;
	      if (ec.audit_features[fst].size() > 0)
		a = & ec.audit_features[fst][j];
	      audit_quad(all, ec.atomics[fst][j], a, ec.atomics[snd], ec.audit_features[snd], features, ns_pre);
	    }
	}

      for (vector<string>::iterator i = all.triples.begin(); i != all.triples.end();i++) 
	{
	  int fst = (*i)[0];
	  int snd = (*i)[1];
	  int trd = (*i)[2];
	  for (size_t j = 0; j < ec.atomics[fst].size(); j++)
	    {
	      audit_data* a1 = NULL;
	      if (ec.audit_features[fst].size() > 0)
		a1 = & ec.audit_features[fst][j];
	      for (size_t k = 0; k < ec.atomics[snd].size(); k++)
		{
		  audit_data* a2 = NULL;
		  if (ec.audit_features[snd].size() > 0)
		    a2 = & ec.audit_features[snd][k];
		  audit_triple(all, ec.atomics[fst][j], a1, ec.atomics[snd][k], a2, ec.atomics[trd], ec.audit_features[trd], features, ns_pre);
		}
	    }
	}

      sort(features.begin(),features.end());
      if(all.audit){
        for (vector<string_value>::iterator sv = features.begin(); sv!= features.end(); sv++)
	  cout << '\t' << (*sv).s;
        cout << endl;
      }
    }
}

void print_audit_features(vw& all, example& ec)
{
  label_data& ld = *(label_data*)ec.ld;
  if(all.audit)
    print_result(all.stdout_fileno,ld.prediction,-1,ec.tag);
  fflush(stdout);
  print_features(all, ec);
}

float finalize_prediction(vw& all, float ret) 
{
  if ( nanpattern(ret))
    {
      cerr << "NAN prediction in example " << all.sd->example_number + 1 << ", forcing 0.0" << endl;
      return 0.;
    }
  if ( ret > all.sd->max_label )
    return (float)all.sd->max_label;
  if (ret < all.sd->min_label)
    return (float)all.sd->min_label;
  return ret;
}

 struct trunc_data {
   float prediction;
   float gravity;
 };
 
 inline void vec_add_trunc(trunc_data& p, const float fx, float& fw) {
   p.prediction += trunc_weight(fw, p.gravity) * fx;
 }

 inline float trunc_predict(vw& all, example& ec, float gravity)
 {
   label_data* ld = (label_data*)ec.ld;
   trunc_data temp = {ld->initial, gravity};
   foreach_feature<trunc_data, vec_add_trunc>(all, ec, temp);
   return temp.prediction;
 }

template<bool reg_mode_odd>
void predict(gd& g, learner& base, example& ec)
{
  vw& all = *g.all;

  if (reg_mode_odd)
    {
      float gravity = (float)all.sd->gravity;
      ec.partial_prediction = trunc_predict(all, ec, gravity);
    }
  else
    ec.partial_prediction = inline_predict(all, ec);    

  label_data& ld = *(label_data*)ec.ld;
  ld.prediction = finalize_prediction(all, ec.partial_prediction * (float)all.sd->contraction);

  if (all.audit || all.hash_inv)
    print_audit_features(all, ec);
}

  struct norm_data {
    float g;
    float pred_per_update;
    float norm_x;
    power_data pd;
  };

template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
inline void pred_per_update_feature(norm_data& nd, float x, float& fw) {
  weight* w = &fw;
  if(!feature_mask || w[feature_mask]==1.){
    float x2 = x * x;
    if(adaptive)
      w[adaptive] += nd.g * x2;
    if(normalized) {
      float x_abs = fabsf(x);
      if( x_abs > w[normalized] ) {// new scale discovered
	if( w[normalized] > 0. ) {//If the normalizer is > 0 then rescale the weight so it's as if the new scale was the old scale.
	  float rescale = (w[normalized]/x_abs);
	  if (sqrt_rate)
	    w[0] *= (adaptive ? rescale : rescale*rescale);
	  else
	    w[0] *= powf(rescale*rescale, -nd.pd.minus_power_t_norm);
	}
	w[normalized] = x_abs;
      }
      nd.norm_x += x2 / (w[normalized] * w[normalized]);
    }
    float rate_decay = compute_rate_decay<sqrt_rate, adaptive, normalized>(nd.pd, fw);

    nd.pred_per_update += x2 * rate_decay;
  }
}
  
template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
float pred_per_update(vw& all, example& ec)
{//We must traverse the features in _precisely_ the same order as during training.
  label_data* ld = (label_data*)ec.ld;
  float g = all.loss->getSquareGrad(ld->prediction, ld->label) * ld->weight;
  if (g==0) return 1.;

  float minus_power_t_norm = (adaptive ? all.power_t : 0.f) - 1.f;
  norm_data nd = {g, 0., 0., {-all.power_t, minus_power_t_norm}};
  
  foreach_feature<norm_data,pred_per_update_feature<sqrt_rate, adaptive, normalized, feature_mask> >(all, ec, nd);
  
  if(normalized) {
    float total_weight = ec.example_t;
    
    if(!all.holdout_set_off)
      total_weight -= (float)all.sd->weighted_holdout_examples; //exclude weights from test_only examples   
    
    all.normalized_sum_norm_x += ld->weight * nd.norm_x;
    
    float avg_norm = all.normalized_sum_norm_x / total_weight;
    if(sqrt_rate) {
      if(adaptive) nd.pred_per_update /= sqrt(avg_norm);
      else nd.pred_per_update /= avg_norm;
    } else 
      nd.pred_per_update *= powf(avg_norm,minus_power_t_norm);
  }
  
  return nd.pred_per_update;
}

template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
void compute_update(vw& all, gd& g, example& ec)
{
  label_data* ld = (label_data*)ec.ld;

  if(g.active_simulation){
    float k = ec.example_t - ld->weight;
    ec.revert_weight = all.loss->getRevertingWeight(all.sd, ld->prediction, all.eta/powf(k,all.power_t));
    float importance = query_decision(all, ec, k);
    if(importance > 0){
      all.sd->queries += 1;
      ld->weight *= importance;
    }
    else //do not query => do not train
      ld->label = FLT_MAX;
  }

  float t;
  if(all.active && ld->label != FLT_MAX)
    t = (float)all.sd->weighted_unlabeled_examples;
  else
    t = (float)(ec.example_t - all.sd->weighted_holdout_examples);

  ec.eta_round = 0;

  if (ld->label != FLT_MAX)
    ec.loss = all.loss->getLoss(all.sd, ld->prediction, ld->label) * ld->weight;

  if (ld->label != FLT_MAX && !ec.test_only)
    {
      if (all.training && ec.loss > 0.)
        {
	  float eta_t;
	  float norm;
          if(adaptive || normalized)
	    norm = pred_per_update<sqrt_rate, adaptive, normalized, feature_mask>(all,ec);
          else
            norm = ec.total_sum_feat_sq;

          eta_t = all.eta * norm * ld->weight;
          if(!adaptive && all.power_t != 0) eta_t *= powf(t,-all.power_t);

          float update = 0.f;
          if( all.invariant_updates )
            update = all.loss->getUpdate(ld->prediction, ld->label, eta_t, norm);
          else
            update = all.loss->getUnsafeUpdate(ld->prediction, ld->label, eta_t, norm);

	  if (all.reg_mode && fabs(ec.eta_round) > 1e-8) {
	    double dev1 = all.loss->first_derivative(all.sd, ld->prediction, ld->label);
	    double eta_bar = (fabs(dev1) > 1e-8) ? (-ec.eta_round / dev1) : 0.0;
	    if (fabs(dev1) > 1e-8)
	      all.sd->contraction *= (1. - all.l2_lambda * eta_bar);
	    all.sd->gravity += eta_bar * all.l1_lambda;
	  }
	  ec.eta_round = (float) (update / all.sd->contraction);
        }
    }
  else if(all.active)
    ec.revert_weight = all.loss->getRevertingWeight(all.sd, ld->prediction, all.eta/powf(t,all.power_t));

}

template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
void update(gd& g, learner& base, example& ec)
{
  vw* all = g.all;

  compute_update<sqrt_rate, adaptive, normalized, feature_mask> (*all, g, ec);
  
  if (ec.eta_round != 0.)
    {
      train<sqrt_rate, adaptive, normalized, feature_mask>(*all,ec,(float)ec.eta_round);
      
      if (all->sd->contraction < 1e-10)  // updating weights now to avoid numerical instability
	sync_weights(*all);
    }
}

template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t feature_mask>
void learn(gd& g, learner& base, example& ec)
{
  vw* all = g.all;
  label_data* ld = (label_data*)ec.ld;

  assert(ec.in_use);

  g.predict(g,base,ec);

  if ((all->holdout_set_off || !ec.test_only) && ld->weight > 0)
    update<sqrt_rate, adaptive, normalized, feature_mask>(g,base,ec);
  else if(ld->weight > 0)
    ec.loss = all->loss->getLoss(all->sd, ld->prediction, ld->label) * ld->weight;
}

void sync_weights(vw& all) {
  if (all.sd->gravity == 0. && all.sd->contraction == 1.)  // to avoid unnecessary weight synchronization
    return;
  uint32_t length = 1 << all.num_bits;
  size_t stride = 1 << all.reg.stride_shift;
  for(uint32_t i = 0; i < length && all.reg_mode; i++)
    all.reg.weight_vector[stride*i] = trunc_weight(all.reg.weight_vector[stride*i], (float)all.sd->gravity) * (float)all.sd->contraction;
  all.sd->gravity = 0.;
  all.sd->contraction = 1.;
}

void save_load_regressor(vw& all, io_buf& model_file, bool read, bool text)
{
  uint32_t length = 1 << all.num_bits;
  uint32_t stride = 1 << all.reg.stride_shift;
  int c = 0;
  uint32_t i = 0;
  size_t brw = 1;

  if(all.print_invert){ //write readable model with feature names           
    weight* v;
    char buff[512];
    int text_len; 
    typedef std::map< std::string, size_t> str_int_map;  
        
    for(str_int_map::iterator it = all.name_index_map.begin(); it != all.name_index_map.end(); ++it){              
      v = &(all.reg.weight_vector[stride*(it->second)]);
      if(*v != 0.){
        text_len = sprintf(buff, "%s", (char*)it->first.c_str());
        brw = bin_text_write_fixed(model_file, (char*)it->first.c_str(), sizeof(*it->first.c_str()),
					 buff, text_len, true);
        text_len = sprintf(buff, ":%f\n", *v);
        brw+= bin_text_write_fixed(model_file,(char *)v, sizeof (*v),
					 buff, text_len, true);
      }	
    }
    return;
  } 

  do 
    {
      brw = 1;
      weight* v;
      if (read)
	{
	  c++;
	  brw = bin_read_fixed(model_file, (char*)&i, sizeof(i),"");
	  if (brw > 0)
	    {
	      assert (i< length);		
	      v = &(all.reg.weight_vector[stride*i]);
	      brw += bin_read_fixed(model_file, (char*)v, sizeof(*v), "");
	    }
	}
      else// write binary or text
	{
                      
         v = &(all.reg.weight_vector[stride*i]);
	 if (*v != 0.)
	    {
	      c++;
	      char buff[512];
	      int text_len;

	      text_len = sprintf(buff, "%d", i);
	      brw = bin_text_write_fixed(model_file,(char *)&i, sizeof (i),
					 buff, text_len, text);
	      
              text_len = sprintf(buff, ":%f\n", *v);
	      brw+= bin_text_write_fixed(model_file,(char *)v, sizeof (*v),
					 buff, text_len, text);
	    }
	}
 
      if (!read)
	i++;
    }
  while ((!read && i < length) || (read && brw >0));  
}

void save_load_online_state(vw& all, io_buf& model_file, bool read, bool text)
{
  char buff[512];
  
  uint32_t text_len = sprintf(buff, "initial_t %f\n", all.initial_t);
  bin_text_read_write_fixed(model_file,(char*)&all.initial_t, sizeof(all.initial_t), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "norm normalizer %f\n", all.normalized_sum_norm_x);
  bin_text_read_write_fixed(model_file,(char*)&all.normalized_sum_norm_x, sizeof(all.normalized_sum_norm_x), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "t %f\n", all.sd->t);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->t, sizeof(all.sd->t), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "sum_loss %f\n", all.sd->sum_loss);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->sum_loss, sizeof(all.sd->sum_loss), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "sum_loss_since_last_dump %f\n", all.sd->sum_loss_since_last_dump);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->sum_loss_since_last_dump, sizeof(all.sd->sum_loss_since_last_dump), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "dump_interval %f\n", all.sd->dump_interval);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->dump_interval, sizeof(all.sd->dump_interval), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "min_label %f\n", all.sd->min_label);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->min_label, sizeof(all.sd->min_label), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "max_label %f\n", all.sd->max_label);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->max_label, sizeof(all.sd->max_label), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "weighted_examples %f\n", all.sd->weighted_examples);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->weighted_examples, sizeof(all.sd->weighted_examples), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "weighted_labels %f\n", all.sd->weighted_labels);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->weighted_labels, sizeof(all.sd->weighted_labels), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "weighted_unlabeled_examples %f\n", all.sd->weighted_unlabeled_examples);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->weighted_unlabeled_examples, sizeof(all.sd->weighted_unlabeled_examples), 
			    "", read, 
			    buff, text_len, text);
  
  text_len = sprintf(buff, "example_number %u\n", (uint32_t)all.sd->example_number);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->example_number, sizeof(all.sd->example_number), 
			    "", read, 
			    buff, text_len, text);

  text_len = sprintf(buff, "total_features %u\n", (uint32_t)all.sd->total_features);
  bin_text_read_write_fixed(model_file,(char*)&all.sd->total_features, sizeof(all.sd->total_features), 
			    "", read, 
			    buff, text_len, text);
  if (!all.training) // reset various things so that we report test set performance properly
    {
      all.sd->sum_loss = 0;
      all.sd->sum_loss_since_last_dump = 0;
      all.sd->dump_interval = 1.;
      all.sd->weighted_examples = 0.;
      all.sd->weighted_labels = 0.;
      all.sd->weighted_unlabeled_examples = 0.;
      all.sd->example_number = 0;
      all.sd->total_features = 0;
    }
  
  uint32_t length = 1 << all.num_bits;
  uint32_t stride = 1 << all.reg.stride_shift;
  int c = 0;
  uint32_t i = 0;
  size_t brw = 1;
  do 
    {
      brw = 1;
      weight* v; 
      if (read)
	{
	  c++;
	  brw = bin_read_fixed(model_file, (char*)&i, sizeof(i),"");
	  if (brw > 0)
	    {
	      assert (i< length);		
	      v = &(all.reg.weight_vector[stride*i]);
	      if (stride == 2) //either adaptive or normalized
		brw += bin_read_fixed(model_file, (char*)v, sizeof(*v)*2, "");
	      else //adaptive and normalized
		brw += bin_read_fixed(model_file, (char*)v, sizeof(*v)*3, "");	
	      if (!all.training)
		v[1]=v[2]=0.;
	    }
	}
      else // write binary or text
	{
	  v = &(all.reg.weight_vector[stride*i]);
	  if (*v != 0.)
	    {
	      c++;
	      char buff[512];
	      int text_len = sprintf(buff, "%d", i);
	      brw = bin_text_write_fixed(model_file,(char *)&i, sizeof (i),
					 buff, text_len, text);
	      
	      if (stride == 2)
		{//either adaptive or normalized
		  text_len = sprintf(buff, ":%f %f\n", *v, *(v+1));
		  brw+= bin_text_write_fixed(model_file,(char *)v, 2*sizeof (*v),
					     buff, text_len, text);
		}
	      else
		{//adaptive and normalized
		  text_len = sprintf(buff, ":%f %f %f\n", *v, *(v+1), *(v+2));
		  brw+= bin_text_write_fixed(model_file,(char *)v, 3*sizeof (*v),
					     buff, text_len, text);
		}
	    }
	}
      if (!read)
	i++;
    }
  while ((!read && i < length) || (read && brw >0));  
}

void save_load(gd& g, io_buf& model_file, bool read, bool text)
{
  vw* all = g.all;
  if(read)
    {
      initialize_regressor(*all);

      if(all->adaptive && all->initial_t > 0)
	{
	  uint32_t length = 1 << all->num_bits;
	  uint32_t stride = 1 << all->reg.stride_shift;
	  for (size_t j = 1; j < stride*length; j+=stride)
	    {
	      all->reg.weight_vector[j] = all->initial_t;   //for adaptive update, we interpret initial_t as previously seeing initial_t fake datapoints, all with squared gradient=1
	      //NOTE: this is not invariant to the scaling of the data (i.e. when combined with normalized). Since scaling the data scales the gradient, this should ideally be 
	      //feature_range*initial_t, or something like that. We could potentially fix this by just adding this base quantity times the current range to the sum of gradients 
	      //stored in memory at each update, and always start sum of gradients to 0, at the price of additional additions and multiplications during the update...
	    }
	}

      if (g.initial_constant != 0.0)
        VW::set_weight(*all, constant, 0, g.initial_constant);

    }

  if (model_file.files.size() > 0)
    {
      bool resume = all->save_resume;
      char buff[512];
      uint32_t text_len = sprintf(buff, ":%d\n", resume);
      bin_text_read_write_fixed(model_file,(char *)&resume, sizeof (resume),
				"", read,
				buff, text_len, text);
      if (resume)
	save_load_online_state(*all, model_file, read, text);
      else
	save_load_regressor(*all, model_file, read, text);
    }
}

template<bool sqrt_rate, size_t adaptive, size_t normalized, size_t next>
size_t set_learn(vw& all, learner* ret, bool feature_mask_off)
{
  all.normalized_idx = normalized;
  if (feature_mask_off)
    {
      ret->set_learn<gd, learn<sqrt_rate, adaptive,normalized,0> >();
      ret->set_update<gd, update<sqrt_rate, adaptive,normalized,0> >();
      all.feature_mask_idx = 0;
      return next;
    }
  else
    {
      ret->set_learn<gd, learn<sqrt_rate, adaptive,normalized,next> >();
      ret->set_update<gd, update<sqrt_rate, adaptive,normalized,next> >();
      all.feature_mask_idx = next;
      return next+1;
    }
}

template<bool sqrt_rate, size_t adaptive>
size_t set_learn(vw& all, learner* ret, bool feature_mask_off)
{
  // select the appropriate learn function based on adaptive, normalization, and feature mask
  if (all.normalized_updates)
    return set_learn<sqrt_rate, adaptive, adaptive+1, adaptive+2>(all, ret, feature_mask_off);
  else
    return set_learn<sqrt_rate, adaptive, 0, adaptive+1>(all, ret, feature_mask_off);
}

template<bool sqrt_rate>
size_t set_learn(vw& all, learner* ret, bool feature_mask_off)
{
  if (all.adaptive)
    return set_learn<sqrt_rate, 1>(all, ret, feature_mask_off);
  else
    return set_learn<sqrt_rate, 0>(all, ret, feature_mask_off);
}

size_t ceil_log_2(size_t v)
{
  if (v==0)
    return 0;
  else 
    return 1 + ceil_log_2(v >> 1);
}

learner* setup(vw& all, po::variables_map& vm)
{
  gd* g = (gd*)calloc_or_die(1, sizeof(gd));
  g->all = &all;
  g->active = all.active;
  g->active_simulation = all.active_simulation;
  g->normalized_sum_norm_x = all.normalized_sum_norm_x;
  g->no_win_counter = 0;
  g->early_stop_thres = 3;

  bool feature_mask_off = true;
  if(vm.count("feature_mask"))
    feature_mask_off = false;

  if(!all.holdout_set_off)
  {
    all.sd->holdout_best_loss = FLT_MAX;
    if(vm.count("early_terminate"))      
      g->early_stop_thres = vm["early_terminate"].as< size_t>();     
  }

  if (vm.count("constant")) {
      g->initial_constant = vm["constant"].as<float>();     
  }

  if( !all.training || ( ( vm.count("sgd") || vm.count("adaptive") || vm.count("invariant") || vm.count("normalized") ) && !vm.count("exact_adaptive_norm")) )
    {//nondefault
      all.adaptive = all.training && vm.count("adaptive");
      all.invariant_updates = all.training && vm.count("invariant");
      all.normalized_updates = all.training && vm.count("normalized");
      
      if(!vm.count("learning_rate") && !vm.count("l") && !(all.adaptive && all.normalized_updates))
	all.eta = 10; //default learning rate to 10 for non default update rule
      
      //if not using normalized or adaptive, default initial_t to 1 instead of 0
      if(!all.adaptive && !all.normalized_updates){
	if (!vm.count("initial_t")) {
	  all.sd->t = 1.f;
	  all.sd->weighted_unlabeled_examples = 1.f;
	  all.initial_t = 1.f;
	}
	all.eta *= powf((float)(all.sd->t), all.power_t);
      }
    }
  
  if (pow((double)all.eta_decay_rate, (double)all.numpasses) < 0.0001 )
    cerr << "Warning: the learning rate for the last pass is multiplied by: " << pow((double)all.eta_decay_rate, (double)all.numpasses)
	 << " adjust --decay_learning_rate larger to avoid this." << endl;
  
  learner* ret = new learner(g, 1);

  if (all.reg_mode % 2)
    {
      ret->set_predict<gd, predict<true> >();
      g->predict = predict<true>;
    }
  else
    {
      ret->set_predict<gd, predict<false> >();
      g->predict = predict<true>;
    }
  
  size_t stride;
  if (all.power_t == 0.5)
    stride = set_learn<true>(all, ret, feature_mask_off);
  else
    stride = set_learn<false>(all, ret, feature_mask_off);

  all.reg.stride_shift = ceil_log_2(stride-1);
  ret->increment = (1 << all.reg.stride_shift);

  ret->set_save_load<gd,save_load>();

  ret->set_end_pass<gd, end_pass>();
  return ret;
}
}
