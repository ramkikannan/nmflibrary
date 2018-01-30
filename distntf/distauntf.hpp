#ifndef DISTNTF_DISTAUNTF_HPP
#define DISTNTF_DISTAUNTF_HPP

#include <armadillo>
#include "ntf_utils.h"
#include "luc.hpp"
#include "distntftime.hpp"
#include "dimtree/kobydt.hpp"

/*
* Tensor A of size is M1 x M2 x... x Mn is distributed among
* P1 x P2 x ... x Pn grid of P processors. That means, every
* processor has (M1/P1) x (M2/P2) x ... x (Mn/Pn) tensor as
* m_input_tensor. Similarly every process own a portion of the
* factors as H(i,pi) of size (Mi/Pi) x k
* and collects from its neighbours as H(i,p) as (Mi/P) x k
* H(i,p) and m_input_tensor can perform local MTTKRP. The
* local MTTKRP's are reduced scattered for local NNLS.
*/

// #define DISTNTF_VERBOSE 1

namespace planc {

#define TENSOR_LOCAL_DIM (m_input_tensor.dimensions())
#define TENSOR_LOCAL_NUMEL (m_input_tensor.numel())

class DistAUNTF {
  private:
    const Tensor m_input_tensor;
    // local ncp factors
    NCPFactors m_local_ncp_factors;
    NCPFactors m_local_ncp_factors_t;
    NCPFactors m_gathered_ncp_factors;
    NCPFactors m_gathered_ncp_factors_t;
    // mttkrp related variables
    MAT *ncp_krp;
    MAT *ncp_mttkrp_t;
    MAT *ncp_local_mttkrp_t;
    // gram related variables.
    MAT factor_local_grams;  // U in the algorithm.
    MAT *factor_global_grams;  // G in the algorithm
    MAT global_gram;  // hadamard of the global_grams

    // NTF related variable.
    const int m_low_rank_k;
    const int m_modes;
    const algotype m_updalgo;
    const UVEC m_global_dims;
    const UVEC m_factor_local_dims;
    int m_num_it;
    int current_mode;
    int current_it;
    FVEC m_regularizers;
    bool m_compute_error;
    bool m_enable_dim_tree;

    // update function
    LUC *m_luc_ntf_update;


    // communication related variables
    const NTFMPICommunicator &m_mpicomm;
    // stats
    DistNTFTime time_stats;

    // computing error related;
    double m_global_sqnorm_A;
    MAT hadamard_all_grams;

    KobyDimensionTree *kdt;
    // do the local syrk only for the current updated factor
    // and all reduce only for the current updated factor.
    // computes G^(current_mode)
    void update_global_gram(const int current_mode) {
        // computing U
        mpitic();  // gram
        // force a ssyrk instead of gemm.
        factor_local_grams = m_local_ncp_factors.factor(current_mode).t() *
                             m_local_ncp_factors.factor(current_mode);
        double temp = mpitoc();  // gram
        this->time_stats.compute_duration(temp);
        this->time_stats.gram_duration(temp);
        factor_global_grams[current_mode].zeros();
        // Computing G.
        mpitic();  // allreduce gram
        MPI_Allreduce(factor_local_grams.memptr(),
                      factor_global_grams[current_mode].memptr(),
                      this->m_low_rank_k * this->m_low_rank_k, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
#ifdef __WITH__BARRIER__TIMING__
        MPI_Barrier(MPI_COMM_WORLD);
#endif
        temp = mpitoc();  // allreduce gram
        applyReg(this->m_regularizers(current_mode * 2),
                 this->m_regularizers(current_mode * 2 + 1),
                 &(factor_global_grams[current_mode]));
        this->time_stats.communication_duration(temp);
        this->time_stats.allreduce_duration(temp);
    }

    void applyReg(float lambda_l2, float lambda_l1, MAT *AtA) {
        // Frobenius norm regularization
        if (lambda_l2 > 0) {
            MAT  identity  = arma::eye<MAT>(this->m_low_rank_k,
                                            this->m_low_rank_k);
            (*AtA) = (*AtA) + 2 * lambda_l2 * identity;
        }

        // L1 - norm regularization
        if (lambda_l1 > 0) {
            MAT  onematrix = arma::ones<MAT>(this->m_low_rank_k,
                                             this->m_low_rank_k);
            (*AtA) = (*AtA) + 2 * lambda_l1 * onematrix;
        }
    }

    /*
    * This iterates over all grams and find hadamard of the grams
    */
    void gram_hadamard(int current_mode) {
        global_gram.ones();
        mpitic();
        for (int i = 0; i < m_modes; i++) {
            if (i != current_mode) {
                //%= element-wise multiplication
                global_gram %= factor_global_grams[i];
            }
        }
        double temp = mpitoc();  // gram
        this->time_stats.compute_duration(temp);
        this->time_stats.gram_duration(temp);
    }

    // factor matrices all gather only on the current update factor
    void gather_ncp_factor(const int current_mode) {
        int sendcnt = m_local_ncp_factors.factor(current_mode).n_elem;
        int recvcnt = m_local_ncp_factors.factor(current_mode).n_elem;
        m_gathered_ncp_factors_t.factor(current_mode).zeros();
        // Had this comment for debugging memory corruption in all_gather
        // DISTPRINTINFO("::ncp_krp::" << ncp_krp[current_mode].memptr()
        //               << "::size::" << ncp_krp[current_mode].n_rows
        //               << "x" << ncp_krp[current_mode].n_cols
        //               << "::m_gathered_ncp_factors_t::"
        //               << m_gathered_ncp_factors_t.factor(current_mode).memptr()
        //               << "::diff from recvcnt::"
        //               << m_gathered_ncp_factors_t.factor(current_mode).memptr() - recvcnt * 8);
        MPI_Comm current_fiber_comm = this->m_mpicomm.fiber(current_mode);
#ifdef DISTNTF_VERBOSE
        int fiber_size;
        // MPI_Comm current_slice_comm = this->m_mpicomm.slice(current_mode);
        // int slice_size;

        MPI_Comm_size(current_fiber_comm, &fiber_size);
        //MPI_Comm_size(current_slice_comm, &slice_size);
        DISTPRINTINFO("::current_mode::" << current_mode
                      << "::fiber comm size::" << fiber_size
                      << "::my_global_rank::" << MPI_RANK
                      << "::my_slice_rank::" << this->m_mpicomm.slice_rank(current_mode)
                      << "::my_fiber_rank::" << this->m_mpicomm.fiber_rank(current_mode)
                      << "::sendcnt::" << sendcnt << "::recvcnt::" << recvcnt
                      << "::gathered factor size::"
                      << m_gathered_ncp_factors_t.factor(current_mode).n_elem);
#endif
        mpitic();  // allgather tic
        MPI_Allgather(m_local_ncp_factors_t.factor(current_mode).memptr(),
                      sendcnt, MPI_DOUBLE,
                      m_gathered_ncp_factors_t.factor(current_mode).memptr(),
                      recvcnt, MPI_DOUBLE,
                      // todo:: check whether it is slice or fiber while running
                      // and debugging the code.
                      current_fiber_comm);
        // current_slice_comm);
        double temp = mpitoc();  // allgather toc
#ifdef DISTNTF_VERBOSE
        DISTPRINTINFO("sent local factor::" << std::endl
                      << m_local_ncp_factors_t.factor(current_mode) << std::endl
                      << " gathered factor::" << std::endl
                      << m_gathered_ncp_factors_t.factor(current_mode));
#endif
        this->time_stats.communication_duration(temp);
        this->time_stats.allgather_duration(temp);
        // keep gather_ncp_factors_t consistent.
        mpitic();  // transpose tic
        m_gathered_ncp_factors.set(current_mode,
                                   m_gathered_ncp_factors_t.factor(current_mode).t());
        temp = mpitoc();
        this->time_stats.compute_duration(temp);
        this->time_stats.trans_duration(temp);
    }

    void distmttkrp(const int &current_mode) {
        double temp;
        if (!this->m_enable_dim_tree) {
            mpitic();
            m_gathered_ncp_factors.krp_leave_out_one(current_mode,
                    &ncp_krp[current_mode]);
            temp = mpitoc();
            this->time_stats.compute_duration(temp);
            this->time_stats.krp_duration(temp);
        }
        mpitic();
        if (this->m_enable_dim_tree) {
            kdt->in_order_reuse_MTTKRP(current_mode,
                                       ncp_mttkrp_t[current_mode].memptr(),
                                       false);

        } else {
            m_input_tensor.mttkrp(current_mode, ncp_krp[current_mode],
                                  &ncp_mttkrp_t[current_mode]);
        }
        // verify if the dimension tree output matches with the classic one
        // MAT kdt_ncp_mttkrp_t = ncp_mttkrp_t[current_mode];
        // bool same_mttkrp = arma::approx_equal(kdt_ncp_mttkrp_t, ncp_mttkrp_t[current_mode], "absdiff", 1e-3);
        // PRINTROOT("kdt vs mttkrp_t::" << same_mttkrp);
        // MAT ncp_mttkrp = ncp_mttkrp_t[current_mode].t();
        // same_mttkrp = arma::approx_equal(kdt_ncp_mttkrp_t, ncp_mttkrp, "absdiff", 1e-3);
        // PRINTROOT("kdt vs mttkrp::" << same_mttkrp);
        // PRINTROOT("kdt mttkrp::" << kdt_ncp_mttkrp_t);
        // PRINTROOT("classic mttkrp_t::" << ncp_mttkrp_t[current_mode]);
        temp = mpitoc();  // mttkrp
        this->time_stats.compute_duration(temp);
        this->time_stats.mttkrp_duration(temp);
        MPI_Comm current_slice_comm = this->m_mpicomm.slice(current_mode);
        int slice_size;
        MPI_Comm_size(current_slice_comm, &slice_size);
        std::vector<int> recvmttkrpsize(slice_size, ncp_local_mttkrp_t[current_mode].n_cols);
#ifdef DISTNTF_VERBOSE
        DISTPRINTINFO("::current_mode::" << current_mode
                      << "::slice comm size::" << slice_size
                      << "::my_global_rank::" << MPI_RANK
                      << "::my_slice_rank::" << this->m_mpicomm.slice_rank(current_mode)
                      << "::my_fiber_rank::" << this->m_mpicomm.fiber_rank(current_mode)
                      << "::mttkrp_size::" << ncp_mttkrp_t[current_mode].n_elem
                      << "::local_mttkrp_size::" << ncp_local_mttkrp_t[current_mode].n_elem);
#endif
        mpitic();  // reduce_scatter mttkrp
        MPI_Reduce_scatter(ncp_mttkrp_t[current_mode].memptr(),
                           ncp_local_mttkrp_t[current_mode].memptr(),
                           &recvmttkrpsize[0],
                           MPI_DOUBLE, MPI_SUM,
                           current_slice_comm);
        temp = mpitoc();  // reduce_scatter mttkrp
#ifdef DISTNTF_VERBOSE
        DISTPRINTINFO(ncp_mttkrp_t[current_mode]);
        DISTPRINTINFO(ncp_local_mttkrp_t[current_mode]);
#endif
        this->time_stats.communication_duration(temp);
        this->time_stats.reducescatter_duration(temp);
    }

    void allocateMatrices() {
        //allocate matrices.
        ncp_krp = new MAT[m_modes];
        ncp_mttkrp_t = new MAT[m_modes];
        ncp_local_mttkrp_t = new MAT[m_modes];
        factor_global_grams = new MAT[m_modes];
        factor_local_grams.zeros(this->m_low_rank_k, this->m_low_rank_k);
        global_gram.ones(this->m_low_rank_k, this->m_low_rank_k);
        UWORD current_size = 0;
        for (int i = 0; i < m_modes; i++) {
            current_size = TENSOR_LOCAL_NUMEL / TENSOR_LOCAL_DIM[i];
            ncp_krp[i] = arma::zeros(current_size, this->m_low_rank_k);
            ncp_mttkrp_t[i] = arma::zeros(this->m_low_rank_k, TENSOR_LOCAL_DIM[i]);
            ncp_local_mttkrp_t[i] = arma::zeros(m_local_ncp_factors.factor(i).n_cols,
                                                m_local_ncp_factors.factor(i).n_rows);
            factor_global_grams[i] = arma::zeros(this->m_low_rank_k,
                                                 this->m_low_rank_k);
        }
    }

    void freeMatrices() {
        for (int i = 0; i < m_modes; i++) {
            ncp_krp[i].clear();
            ncp_mttkrp_t[i].clear();
            ncp_local_mttkrp_t[i].clear();
            factor_global_grams[i].clear();
        }
        delete[] ncp_krp;
        delete[] ncp_mttkrp_t;
        delete[] ncp_local_mttkrp_t;
        delete[] factor_global_grams;
    }

    void reportTime(const double temp, const std::string &reportstring) {
        double mintemp, maxtemp, sumtemp;
        MPI_Allreduce(&temp, &maxtemp, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
        MPI_Allreduce(&temp, &mintemp, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
        MPI_Allreduce(&temp, &sumtemp, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        PRINTROOT(reportstring \
                  // << "::dims::" << this->m_global_dims.t()
                  << "::k::" << this->m_low_rank_k << "::SIZE::" << MPI_SIZE \
                  << "::algo::" << this->m_updalgo \
                  << "::root::" << temp \
                  << "::min::" << mintemp \
                  << "::avg::" << (sumtemp) / (MPI_SIZE) \
                  << "::max::" << maxtemp);
    }

    void generateReport() {
        MPI_Barrier(MPI_COMM_WORLD);
        this->reportTime(this->time_stats.duration(), "total_d");
        this->reportTime(this->time_stats.communication_duration(), "total_comm");
        this->reportTime(this->time_stats.compute_duration(), "total_comp");
        this->reportTime(this->time_stats.allgather_duration(), "total_allgather");
        this->reportTime(this->time_stats.allreduce_duration(), "total_allreduce");
        this->reportTime(this->time_stats.reducescatter_duration(),
                         "total_reducescatter");
        this->reportTime(this->time_stats.gram_duration(), "total_gram");
        this->reportTime(this->time_stats.krp_duration(), "total_mttkrp");
        this->reportTime(this->time_stats.mttkrp_duration(), "total_mttkrp");
        this->reportTime(this->time_stats.nnls_duration(), "total_nnls");
        if (this->m_compute_error) {
            this->reportTime(this->time_stats.err_compute_duration(),
                             "total_err_compute");
            this->reportTime(this->time_stats.err_compute_duration(),
                             "total_err_communication");
        }
    }

  public:
    DistAUNTF(const Tensor &i_tensor, const int i_k, algotype i_algo,
              const UVEC &i_global_dims,
              const UVEC &i_local_dims,
              const NTFMPICommunicator &i_mpicomm) :
        m_input_tensor(i_tensor.dimensions(), i_tensor.m_data),
        m_low_rank_k(i_k),
        m_updalgo(i_algo),
        m_mpicomm(i_mpicomm),
        m_modes(m_input_tensor.modes()),
        m_global_dims(i_global_dims),
        m_factor_local_dims(i_local_dims),
        m_local_ncp_factors(i_local_dims, i_k, false),
        m_local_ncp_factors_t(i_local_dims, i_k, true),
        m_gathered_ncp_factors(i_tensor.dimensions(), i_k, false),
        m_gathered_ncp_factors_t(i_tensor.dimensions(), i_k, true),
        time_stats(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) {
        this->m_compute_error = false;
        this->m_enable_dim_tree = false;
        this->m_num_it = 30;
        //local factors.
        arma::arma_rng::set_seed(i_mpicomm.rank());
        m_local_ncp_factors.distributed_normalize();
        for (int i = 0; i < this->m_modes; i++) {
            MAT current_factor = arma::trans(m_local_ncp_factors.factor(i));
            m_local_ncp_factors_t.set(i, current_factor);
        }
        m_gathered_ncp_factors.trans(m_gathered_ncp_factors_t);
        m_luc_ntf_update = new LUC();
        allocateMatrices();
        double normA = i_tensor.norm();
        MPI_Allreduce(&normA,
                      &this->m_global_sqnorm_A,
                      1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    }
    ~DistAUNTF() {
        freeMatrices();
        if (this->m_enable_dim_tree) {
            delete kdt;
        }
    }
    void num_iterations(const int i_n) { this->m_num_it = i_n;}
    void regularizers(const FVEC i_regs) {this->m_regularizers = i_regs;}
    void compute_error(bool i_error) {
        this->m_compute_error = i_error;
        hadamard_all_grams = arma::ones<MAT>(this->m_low_rank_k,
                                             this->m_low_rank_k);
    }
    void dim_tree(bool i_dim_tree) {
        this->m_enable_dim_tree = i_dim_tree;
        if (this->m_enable_dim_tree) {
            if (this->ncp_krp != NULL) {
                for (int i = 0; i < m_modes; i++) {
                    ncp_krp[i].clear();
                }
                delete[] ncp_krp;
            }
        }
    }
    void computeNTF() {
        // initialize everything.
        // line 3,4,5 of the algorithm
        for (int i = 1; i < m_modes; i++) {
            update_global_gram(i);
            gather_ncp_factor(i);
        }
        if (this->m_enable_dim_tree) {
            kdt = new KobyDimensionTree(m_input_tensor, m_gathered_ncp_factors,
                                        m_input_tensor.modes() / 2);
        }
#ifdef DISTNTF_VERBOSE
        DISTPRINTINFO("local factor matrices::");
        this->m_local_ncp_factors.print();
        DISTPRINTINFO("local factor matrices transpose::");
        this->m_local_ncp_factors_t.print();
        DISTPRINTINFO("gathered factor matrices::");
        this->m_gathered_ncp_factors.print();
#endif
        for (int current_it = 0; current_it < m_num_it; current_it++) {
            MAT unnorm_factor;
            for (int current_mode = 0; current_mode < m_modes; current_mode++) {
                // line 9 and 10 of the algorithm
                distmttkrp(current_mode);
                // line 11 of the algorithm
                gram_hadamard(current_mode);
                // line 12 of the algorithm
                mpitic();  // nnls_tic
#ifdef DISTNTF_VERBOSE
                DISTPRINTINFO("local factor matrix::"
                              << this->m_local_ncp_factors.factor(current_mode));
                DISTPRINTINFO("gathered factor matrix::");
                this->m_gathered_ncp_factors.print();
                PRINTROOT("global_grams::" << std::endl << this->global_gram);
                DISTPRINTINFO("mttkrp::");
                this->ncp_local_mttkrp_t[current_mode].print();
#endif
                MAT factor = m_luc_ntf_update->update(m_updalgo, global_gram,
                                                      ncp_local_mttkrp_t[current_mode]);
#ifdef DISTNTF_VERBOSE
                DISTPRINTINFO("it::" << current_it << "::mode::" << current_mode
                              << std::endl << factor);
#endif
                double temp = mpitoc();  // nnls_toc
                this->time_stats.nnls_duration(temp);
                if (m_compute_error && current_mode == this->m_modes - 1) {
                    unnorm_factor = factor;
                }
                m_local_ncp_factors.set(current_mode, factor.t());
                m_local_ncp_factors.distributed_normalize(current_mode);
                factor = m_local_ncp_factors.factor(current_mode).t();
                m_local_ncp_factors_t.set(current_mode, factor);
                // line 13 and 14
                update_global_gram(current_mode);
                // line 15
                gather_ncp_factor(current_mode);
                if (this->m_enable_dim_tree) {
                    kdt->set_factor(m_gathered_ncp_factors_t.factor(current_mode).memptr(),
                                    current_mode);
                }
            }
            if (m_compute_error) {
                double temp_err = computeError(unnorm_factor);
                PRINTROOT("Iter::" << current_it << "::relative_error::" << temp_err);
            }
            PRINTROOT("completed it::" << current_it);
        }
        generateReport();
    }
    double computeError(MAT &unnorm_factor) {

        // rel_Error = sqrt(max(init.nr_X^2 + lambda^T * Hadamard of all gram * lambda - 2 * innerprod(X,F_kten),0))/init.nr_X;
        mpitic();
        hadamard_all_grams = global_gram % factor_global_grams[this->m_modes - 1];
        VEC local_lambda = m_local_ncp_factors.lambda();
        PRINTROOT(local_lambda);
        ROWVEC temp_vec = local_lambda.t() * hadamard_all_grams;
        double sq_norm_model = arma::dot(temp_vec, local_lambda);
        // double sq_norm_model = arma::norm(hadamard_all_grams, "fro");
        // sum of the element-wise dot product between the local mttkrp and
        // the factor matrix
        double inner_product = arma::dot(ncp_local_mttkrp_t[this->m_modes - 1],
                                         unnorm_factor);
        double temp = mpitoc();
        this->time_stats.err_compute_duration(temp);
        double all_inner_product;
        mpitic();
        MPI_Allreduce(&inner_product, &all_inner_product, 1, MPI_DOUBLE, MPI_SUM,
                      MPI_COMM_WORLD);
        temp = mpitoc();
        this->time_stats.err_communication_duration(temp);
#ifdef DISTNTF_VERBOSE
        DISTPRINTINFO ("local_inner_product::" << inner_product << std::endl);
        PRINTROOT("norm_A_sq :: " << this->m_global_sqnorm_A
                  << "::model_norm_sq::" << sq_norm_model
                  << "::global_inner_product::" << all_inner_product << std::endl);
#endif
        double squared_err = this->m_global_sqnorm_A + sq_norm_model
                             - 2 * all_inner_product;
        return std::sqrt(squared_err / this->m_global_sqnorm_A);
    }
};  // class DistAUNTF
}  // namespace PLANC
#endif  // DISTNTF_DISTAUNTF_HPP