#include <fstream>
#include <string>
#include <iostream>

#include "io.hh"
#include "str.hh"
#include "conf.hh"
#include "HmmSet.hh"
#include "LmbfgsOptimize.hh"

std::string statistics_file;
std::string out_model_name;
std::string state_file;

int info;

conf::Config config;
HmmSet model;
LmbfgsOptimize optimizer;

double min_var;
double ac_scale;
int num_frames = 0;



// Gets Gaussian parameters, transforms them to optimization form and sets
// the parameters in the optimizer object
void initialize_optimization_parameters(void)
{
  PDFPool *pool = model.get_pool();
  int num_params = 0;
  Vector params;
  
  // Compute first the number of parameters
  for (int i = 0; i < model.num_emission_pdfs(); i++)
    num_params += model.get_emission_pdf(i)->size();
  num_params += pool->size()*pool->dim()*2;
  
  // Allocate parameter vector and fill in the parameters
  params.resize(num_params);
  int pindex = 0;

  // Mixture components
  for (int i = 0; i < model.num_emission_pdfs(); i++)
  {
    Mixture *m = model.get_emission_pdf(i);
    for (int j = 0; j < m->size(); j++)
      params(pindex++) = util::safe_log(m->get_mixture_coefficient(j));
  }

  // Means and covariances (diagonal)
  for (int i = 0; i < pool->size(); i++)
  {
    Gaussian *pdf = dynamic_cast< Gaussian* >(pool->get_pdf(i));
    if (pdf == NULL)
      throw std::string("Only Gaussian PDFs are supported!");
    Vector temp;
    pdf->get_mean(temp);
    assert( temp.size() == pool->dim() );
    for (int j = 0; j < pool->dim(); j++)
      params(pindex++) = temp(j);
    pdf->get_covariance(temp);
    assert( temp.size() == pool->dim() );
    for (int j = 0; j < pool->dim(); j++)
    {
      if (temp(j) < 1.0001*min_var)
        temp(j) = 1.0001*min_var;
      params(pindex++) = log(temp(j)-min_var);
    }
  }
  assert( pindex == num_params );

  optimizer.set_parameters(params);
}


// Moves the parameters from the optimizer object back to the model
void set_model_parameters(void)
{
  int pindex = 0;
  PDFPool *pool = model.get_pool();
  Vector params;

  optimizer.get_parameters(params);
  
  // Mixture components
  for (int i = 0; i < model.num_emission_pdfs(); i++)
  {
    Mixture *m = model.get_emission_pdf(i);
    // Compute the normalization
    double norm = 0;
    for (int j = 0; j < m->size(); j++)
      norm += exp(params(pindex+j));
    for (int j = 0; j < m->size(); j++)
      m->set_mixture_coefficient(j, exp(params(pindex++))/norm);
  }

  // Means and covariances
  for (int i = 0; i < pool->size(); i++)
  {
    Gaussian *pdf = dynamic_cast< Gaussian* >(pool->get_pdf(i));
    if (pdf == NULL)
      throw std::string("Only Gaussian PDFs are supported!");
    Vector temp(pool->dim());
    for (int j = 0; j < pool->dim(); j++)
      temp(j) = params(pindex++);
    pdf->set_mean(temp);
    for (int j = 0; j < pool->dim(); j++)
      temp(j) = min_var + exp(params(pindex++));
    pdf->set_covariance(temp);
  }
}


// Extracts the gradient from model accumulators
void extract_gradient(void)
{
  int pindex = 0;
  PDFPool *pool = model.get_pool();
  Vector params;
  Vector gradient;

  optimizer.get_parameters(params);
  gradient.resize(optimizer.get_num_parameters());
  
  // Mixture components
  for (int i = 0; i < model.num_emission_pdfs(); i++)
  {
    Mixture *m = model.get_emission_pdf(i);
    // Compute the normalization
    double norm = 0;
    for (int j = 0; j < m->size(); j++)
      norm += exp(params(pindex+j));
    for (int j = 0; j < m->size(); j++)
    {
      double ep = exp(params(pindex));
      gradient(pindex) =
        -(ac_scale*m->get_accumulated_gamma(PDF::MPE_NUM_BUF, j) /
          ((double)num_frames * m->get_mixture_coefficient(j)) *
          ((ep - m->get_mixture_coefficient(j)*ep) / norm));
      pindex++;
    }
  }
  
  // Means and covariances
  for (int i = 0; i < pool->size(); i++)
  {
    Gaussian *pdf = dynamic_cast< Gaussian* >(pool->get_pdf(i));
    if (pdf == NULL)
      throw std::string("Only Gaussian PDFs are supported!");
    Vector m1;
    Vector m2;
    Vector mean;
    Vector diag_cov;
    double gamma;
    pdf->get_mean(mean);
    pdf->get_covariance(diag_cov);
    pdf->get_accumulated_mean(PDF::MPE_NUM_BUF, m1);
    pdf->get_accumulated_second_moment(PDF::MPE_NUM_BUF, m2);
    gamma = pdf->get_accumulated_gamma(PDF::MPE_NUM_BUF);
    for (int j = 0; j < pool->dim(); j++)
      gradient(pindex++) =
        -(ac_scale*(m1(j) - mean(j)*gamma) / (diag_cov(j)*(double)num_frames));
    for (int j = 0; j < pool->dim(); j++)
    {
      double ep = exp(params(pindex));
      double c = ep + min_var;
      gradient(pindex) =
        -(ac_scale*(((m2(j)-2*m1(j)*mean(j)+gamma*mean(j)*mean(j))*ep)/(2*c*c)-
                    ep/(2*c)) / (double)num_frames);
      pindex++;
    }
  }
  assert( pindex == optimizer.get_num_parameters() );

  optimizer.set_gradient(gradient);
}


int
main(int argc, char *argv[])
{
  double total_log_likelihood = 0;
  double total_mpe_score = 0;
  double total_mpe_num_score = 0;
  
  try {
    config("usage: optmodel [OPTION...]\n")
      ('h', "help", "", "", "display help")
      ('b', "base=BASENAME", "arg", "", "Previous base filename for model files")
      ('g', "gk=FILE", "arg", "", "Previous mixture base distributions")
      ('m', "mc=FILE", "arg", "", "Previous mixture coefficients for the states")
      ('p', "ph=FILE", "arg", "", "Previous HMM definitions")
      ('L', "list=LISTNAME", "arg must", "", "file with one statistics file per line")
      ('F', "osf=FILE", "arg must", "", "Optimization state file")
      ('o', "out=BASENAME", "arg must", "", "base filename for output models")
      ('l', "initscale=SCALE", "arg", "", "Initialize with inverse Hessian scale")
      ('\0', "minvar=FLOAT", "arg", "0.09", "minimum variance (default 0.09)")
      ('A', "ac-scale=FLOAT", "arg", "1", "acoustic scaling used in stats")
      ('\0', "bfgsu=INT", "arg", "4", "Number of BFGS updates (default 4)")
      ('s', "savesum=FILE", "arg", "", "save summary information (loglikelihood)")
      ('i', "info=INT", "arg", "0", "info level")
      ;
    config.default_parse(argc, argv);

    info = config["info"].get_int();
    out_model_name = config["out"].get_str();

    optimizer.set_verbosity(info);

    // Load the previous models
    if (config["base"].specified)
    {
      model.read_all(config["base"].get_str());
    }
    else if (config["gk"].specified && config["mc"].specified &&
             config["ph"].specified)
    {
      model.read_gk(config["gk"].get_str());
      model.read_mc(config["mc"].get_str());
      model.read_ph(config["ph"].get_str());
    }
    else
    {
      throw std::string("Must give either --base or all --gk, --mc and --ph");
    }

    // Open the list of statistics files
    std::ifstream filelist(config["list"].get_str().c_str());
    if (!filelist)
    {
      fprintf(stderr, "Could not open %s\n", config["list"].get_str().c_str());
      exit(1);
    }

    optimizer.set_max_bfgs_updates(config["bfgsu"].get_int());

    // Accumulate statistics
    while (filelist >> statistics_file && statistics_file != " ") {
      model.accumulate_gk_from_dump(statistics_file+".gks");
      model.accumulate_mc_from_dump(statistics_file+".mcs");
      std::string lls_file_name = statistics_file+".lls";
      std::ifstream lls_file(lls_file_name.c_str());
      if (lls_file)
      {
        double temp;
        int itemp;
        lls_file >> temp;
        total_log_likelihood += temp;
        lls_file >> temp;
        total_mpe_score += temp;
        lls_file >> temp;
        total_mpe_num_score += temp;
        lls_file >> itemp;
        num_frames += itemp;
        lls_file.close();
      }
    }

    state_file = config["osf"].get_str();
    min_var = config["minvar"].get_float();
    ac_scale = config["ac-scale"].get_float();
    
    if (config["initscale"].specified)
    {
      optimizer.set_inv_hessian_scale(config["initscale"].get_float());
      initialize_optimization_parameters();
    }
    else
    {
      // Load optimization state and model parameters
      if (!optimizer.load_optimization_state(state_file))
      {
        fprintf(stderr, "Could not read %s, start optimization with --initscale\n", state_file.c_str());
        exit(1);
      }
    }

    // Change the value from phone accuracy to phone error in order to
    // turn the optimization problem into minimization
    optimizer.set_function_value(1 - (total_mpe_score / (double)num_frames));

    extract_gradient();

    // Perform the optimization step
    optimizer.optimization_step();

    if (!optimizer.converged())
    {
      // Write the resulting models
      set_model_parameters();
      model.write_all(out_model_name);
      // Write the optimization state
      optimizer.write_optimization_state(state_file);
    }
    else
      fprintf(stderr, "The model has converged!\n");
    
    if (config["savesum"].specified) {
      std::string summary_file_name = config["savesum"].get_str();
      std::ofstream summary_file(summary_file_name.c_str(),
                                 std::ios_base::app);
      if (!summary_file)
        fprintf(stderr, "Could not open summary file: %s\n",
                summary_file_name.c_str());
      else
      {
        summary_file << total_log_likelihood << std::endl;
        summary_file << "  " << total_mpe_score << std::endl;
        summary_file << "  " << total_mpe_num_score << std::endl;
        summary_file << "  " << num_frames << std::endl;
      }
      summary_file.close();
    }
  }
  
  catch (std::exception &e) {
    fprintf(stderr, "exception: %s\n", e.what());
    abort();
  }
  
  catch (std::string &str) {
    fprintf(stderr, "exception: %s\n", str.c_str());
    abort();
  }
}