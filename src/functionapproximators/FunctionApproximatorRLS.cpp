/**
 * @file   FunctionApproximatorRLS.cpp
 * @brief  FunctionApproximatorRLS class source file.
 * @author Freek Stulp
 *
 * This file is part of DmpBbo, a set of libraries and programs for the 
 * black-box optimization of dynamical movement primitives.
 * Copyright (C) 2014 Freek Stulp, ENSTA-ParisTech
 * 
 * DmpBbo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * DmpBbo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with DmpBbo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/serialization/export.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include "functionapproximators/FunctionApproximatorRLS.hpp"

/** For boost::serialization. See http://www.boost.org/doc/libs/1_55_0/libs/serialization/doc/special.html#export */
BOOST_CLASS_EXPORT_IMPLEMENT(DmpBbo::FunctionApproximatorRLS);

#include "functionapproximators/ModelParametersRLS.hpp"
#include "functionapproximators/MetaParametersRLS.hpp"
#include "functionapproximators/BasisFunction.hpp"

#include "dmpbbo_io/EigenFileIO.hpp"
#include "dmpbbo_io/EigenBoostSerialization.hpp"

#include <iostream>
#include <eigen3/Eigen/SVD>
#include <eigen3/Eigen/LU>

using namespace std;
using namespace Eigen;

namespace DmpBbo {

FunctionApproximatorRLS::FunctionApproximatorRLS(const MetaParametersRLS *const meta_parameters, const ModelParametersRLS *const model_parameters) 
:
  FunctionApproximator(meta_parameters,model_parameters)
{
  //if (model_parameters!=NULL)
  //  preallocateMemory(model_parameters->getNumberOfBasisFunctions());
}

FunctionApproximatorRLS::FunctionApproximatorRLS(const ModelParametersRLS *const model_parameters) 
:
  FunctionApproximator(model_parameters)
{
  //preallocateMemory(model_parameters->getNumberOfBasisFunctions());
}


/*
void FunctionApproximatorRLS::preallocateMemory(int n_basis_functions)
{
  lines_one_prealloc_ = MatrixXd(1,n_basis_functions);
  activations_one_prealloc_ = MatrixXd(1,n_basis_functions);
  
  lines_prealloc_ = MatrixXd(1,n_basis_functions);
  activations_prealloc_ = MatrixXd(1,n_basis_functions);
}
*/


FunctionApproximator* FunctionApproximatorRLS::clone(void) const {
  // All error checking and cloning is left to the FunctionApproximator constructor.
  return new FunctionApproximatorRLS(
    dynamic_cast<const MetaParametersRLS*>(getMetaParameters()),
    dynamic_cast<const ModelParametersRLS*>(getModelParameters())
    );
};



void FunctionApproximatorRLS::train(const Eigen::Ref<const Eigen::MatrixXd>& inputs, const Eigen::Ref<const Eigen::MatrixXd>& targets)
{
  if (isTrained())  
  {
    cerr << "WARNING: You may not call FunctionApproximatorRLS::train more than once. Doing nothing." << endl;
    cerr << "   (if you really want to retrain, call reTrain function instead)" << endl;
    return;
  }
  
  assert(inputs.rows() == targets.rows());
  assert(inputs.cols()==getExpectedInputDim());

  const MetaParametersRLS* meta_parameters_lwr = 
    dynamic_cast<const MetaParametersRLS*>(getMetaParameters());
  
  double regularization = meta_parameters_lwr->regularization();
  bool use_offset = meta_parameters_lwr->use_offset();
  
  int n_samples = inputs.rows();
  
  // Make the design matrix
  MatrixXd X;
  if (use_offset)
  {
    X = MatrixXd::Ones(inputs.rows(),inputs.cols()+1);
    X.leftCols(inputs.cols()) = inputs;
  }
  else
  {
    X = inputs;
  }
    
  Do we want to do weighted least squares here? If so, how are the weights passed?
    
  Idea: standard predict is without weights, static predict(inputs,weights,targets) is weighted version   
     
  //int n_kernels = activations.cols();
  //int n_samples = X.rows(); 
  int n_betas = X.cols(); 
  MatrixXd W;
  MatrixXd beta(n_kernels,n_betas);
  
  for (int bb=0; bb<n_kernels; bb++)
  {
    VectorXd W_vec = activations.col(bb);
    
    if (epsilon==0)
    {
      // Use all data
      
      W = W_vec.asDiagonal();
      // Compute beta
      // 1 x n_betas 
      // = inv( (n_betas x n_sam)*(n_sam x n_sam)*(n_sam*n_betas) )*( (n_betas x n_sam)*(n_sam x n_sam)*(n_sam * 1) )   
      // = inv(n_betas x n_betas)*(n_betas x 1)
      VectorXd cur_beta = (X.transpose()*W*X).inverse()*X.transpose()*W*targets;
      beta.row(bb)   =    cur_beta;
    } 
    else
    {
      // Very low weights do not contribute to the line fitting
      // Therefore, we can delete the rows in W, X and targets for which W is small
      //
      // Example with epsilon = 0.1 (a very high value!! usually it will be lower)
      //    W =       [0.001 0.01 0.5 0.98 0.46 0.01 0.001]^T
      //    X =       [0.0   0.1  0.2 0.3  0.4  0.5  0.6 ; 
      //               1.0   1.0  1.0 1.0  1.0  1.0  1.0  ]^T  (design matrix, so last column = 1)
      //    targets = [1.0   0.5  0.4 0.5  0.6  0.7  0.8  ]
      //
      // will reduce to
      //    W_sub =       [0.5 0.98 0.46 ]^T
      //    X_sub =       [0.2 0.3  0.4 ; 
      //                   1.0 1.0  1.0  ]^T  (design matrix, last column = 1)
      //    targets_sub = [0.4 0.5  0.6  ]
      // 
      // Why all this trouble? Because the submatrices will often be much smaller than the full
      // ones, so they are much faster to invert (note the .inverse() call)
      
      // Get a vector where 1 represents that W_vec >= epsilon, and 0 otherswise
      VectorXi large_enough = (W_vec.array() >= epsilon).select(VectorXi::Ones(W_vec.size()), VectorXi::Zero(W_vec.size()));

      // Number of samples in the submatrices
      int n_samples_sub = large_enough.sum();
    
      // This would be a 1-liner in Matlab... but Eigen is not good with splicing.
      VectorXd W_vec_sub(n_samples_sub);
      MatrixXd X_sub(n_samples_sub,n_betas);
      MatrixXd targets_sub(n_samples_sub,targets.cols());
      int jj=0;
      for (int ii=0; ii<n_samples; ii++)
      {
        if (large_enough[ii]==1)
        {
          W_vec_sub[jj] = W_vec[ii];
          X_sub.row(jj) = X.row(ii);
          targets_sub.row(jj) = targets.row(ii);
          jj++;
        }
      }
      
      // Do the same inversion as above, but with only a small subset of the data
      MatrixXd W_sub = W_vec_sub.asDiagonal();
      VectorXd cur_beta_sub = (X_sub.transpose()*W_sub*X_sub).inverse()*X_sub.transpose()*W_sub*targets_sub;
   
      //cout << "  n_samples=" << n_samples << endl;
      //cout << "  n_samples_sub=" << n_samples_sub << endl;
      //cout << cur_beta.transpose() << endl;
      //cout << cur_beta_sub.transpose() << endl;
      beta.row(bb)   =    cur_beta_sub;
    }
  }
  MatrixXd offsets = beta.rightCols(1);
  MatrixXd slopes = beta.leftCols(n_betas-1);
  
  setModelParameters(new ModelParametersRLS(centers,widths,slopes,offsets,asym_kernels));
  
  preallocateMemory(n_kernels);
}

void FunctionApproximatorRLS::predict(const Eigen::Ref<const Eigen::MatrixXd>& inputs, MatrixXd& outputs)
{

  if (!isTrained())  
  {
    cerr << "WARNING: You may not call FunctionApproximatorLWPR::predict if you have not trained yet. Doing nothing." << endl;
    return;
  }
  
  // The following line of code took a long time to decide on.
  // The member FunctionApproximator::model_parameters_ (which we access through
  // getModelParameters()) is of class ModelParameters, not ModelParametersRLS.
  // So within this function, we need to cast it to ModelParametersRLS in order to make predictions.
  // There are three options to do this:
  //
  // 1) use a dynamic_cast. This is really the best way to do it, but the execution of dynamic_cast
  //    can take relatively long, so we should avoid calling it in this time-critical function
  //    predict() function. (note: because it doesn't matter so much for the non time-critical
  //    train() function above, we  use the preferred dynamic_cast<MetaParametersRLS*> as we should)
  //
  // 2) move the model_parameters_ member from FunctionApproximator to FunctionApproximatorRLS, and 
  //    make it ModelParametersRLS instead of ModelParameters. This, however, will lead to lots of 
  //    code duplication, because each derived function approximator class will have to do this.
  //
  // 3) Do a static_cast. The static cast does not do checking like dynamic_cast, so we have to be
  //    really sure that getModelParameters returns a ModelParametersRLS. The only way in which this 
  //    could wrong is if someone calls setModelParameters() with a different derived class. And
  //    this is near-impossible, because setModelParameters is protected within 
  //    FunctionApproximator, and a derived class would be really dumb to set ModelParametersAAA 
  //    with setModelParameters and expect getModelParameters to return ModelParametersBBB. 
  //
  // So I decided to go with 3) because it is fast and does not lead to code duplication, 
  // and only real dumb derived classes can cause trouble ;-)
  //
  // Note: The execution time difference between 2) and 3) is negligible:  
  //   No cast    : 8.90 microseconds/prediction of 1 input sample
  //   Static cast: 8.91 microseconds/prediction of 1 input sample
  //
  // There, ~30 lines of comment for one line of code ;-) 
  //                                            (mostly for me to remember why it is like this) 
  const ModelParametersRLS* model_parameters_lwr = static_cast<const ModelParametersRLS*>(getModelParameters());
  
  bool only_one_sample = (inputs.rows()==1);
  if (only_one_sample)
  {
    ENTERING_REAL_TIME_CRITICAL_CODE

    // Only 1 sample, so real-time execution is possible. No need to allocate memory.
    model_parameters_lwr->getLines(inputs, lines_one_prealloc_);

    // Weight the values for each line with the normalized basis function activations  
    model_parameters_lwr->kernelActivations(inputs,activations_one_prealloc_);
  
    outputs = (lines_one_prealloc_.array()*activations_one_prealloc_.array()).rowwise().sum();  
    
    EXITING_REAL_TIME_CRITICAL_CODE
    
  }
  else
  {
    
    int n_time_steps = inputs.rows();
    int n_basis_functions = model_parameters_lwr->getNumberOfBasisFunctions();
    
    // The next two lines are not be real-time, as they allocate memory
    lines_prealloc_.resize(n_time_steps,n_basis_functions);
    activations_prealloc_.resize(n_time_steps,n_basis_functions);
    outputs.resize(n_time_steps,getExpectedOutputDim());
    
    model_parameters_lwr->getLines(inputs, lines_prealloc_);

    // Weight the values for each line with the normalized basis function activations  
    model_parameters_lwr->kernelActivations(inputs,activations_prealloc_);
  
    outputs = (lines_prealloc_.array()*activations_prealloc_.array()).rowwise().sum();  
    
  }
  
}

bool FunctionApproximatorRLS::saveGridData(const VectorXd& min, const VectorXd& max, const VectorXi& n_samples_per_dim, string save_directory, bool overwrite) const
{
  if (save_directory.empty())
    return true;
  
  MatrixXd inputs;
  FunctionApproximator::generateInputsGrid(min, max, n_samples_per_dim, inputs);

  const ModelParametersRLS* model_parameters_lwr = static_cast<const ModelParametersRLS*>(getModelParameters());
  
  int n_samples = inputs.rows();
  int n_basis_functions = model_parameters_lwr->getNumberOfBasisFunctions();
  
  MatrixXd lines(n_samples,n_basis_functions);
  model_parameters_lwr->getLines(inputs, lines);
  
  MatrixXd unnormalized_activations(n_samples,n_basis_functions);
  model_parameters_lwr->unnormalizedKernelActivations(inputs, unnormalized_activations);

  MatrixXd activations(n_samples,n_basis_functions);
  model_parameters_lwr->kernelActivations(inputs, activations);

  MatrixXd predictions = (lines.array()*activations.array()).rowwise().sum();
  
  saveMatrix(save_directory,"n_samples_per_dim.txt",n_samples_per_dim,overwrite);
  saveMatrix(save_directory,"inputs_grid.txt",inputs,overwrite);
  saveMatrix(save_directory,"lines_grid.txt",lines,overwrite);
  saveMatrix(save_directory,"activations_unnormalized_grid.txt",unnormalized_activations,overwrite);
  saveMatrix(save_directory,"activations_grid.txt",activations,overwrite);
  saveMatrix(save_directory,"predictions_grid.txt",predictions,overwrite);

  
  return true;
  
}

template<class Archive>
void FunctionApproximatorRLS::serialize(Archive & ar, const unsigned int version)
{
  // serialize base class information
  ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(FunctionApproximator);
}

}