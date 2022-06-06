#include "derivative.h"
#include "lin_alg.h"
#include "util.h"
#include "auxil.h"
#include "lin_sys.h"
#include "proj.h"
#include "error.h"
#include "csc_utils.h"


c_int adjoint_derivative(OSQPSolver *solver, c_float *dx, c_float *dy_l, c_float *dy_u, const csc* check1, const c_float* check2, c_float tol1, c_float tol2) {

    c_int m = solver->work->data->m;
    c_int n = solver->work->data->n;

    OSQPSettings*  settings  = solver->settings;

    OSQPMatrix *P = solver->work->data->P;
    OSQPMatrix *A = solver->work->data->A;
    OSQPVectorf *l = solver->work->data->l;
    OSQPVectorf *u = solver->work->data->u;
    OSQPVectorf *x = solver->work->x;
    OSQPVectorf *y = solver->work->y;

    c_float *l_data = OSQPVectorf_data(l);
    c_float *u_data = OSQPVectorf_data(u);
    c_float *y_data = OSQPVectorf_data(y);

    c_int *A_ineq_l_vec = (c_int *) c_malloc(m * sizeof(c_int));
    c_int *A_ineq_u_vec = (c_int *) c_malloc(m * sizeof(c_int));
    c_int *A_eq_vec = (c_int *) c_malloc(m * sizeof(c_int));

    c_int *nu_sign_vec = (c_int *) c_malloc(m * sizeof(c_int));

    // TODO: We could use constr_type in OSQPWorkspace but it only tells us whether a constraint is 'loose'
    // not 'upper loose' or 'lower loose', which we seem to need here.
    c_float infval = OSQP_INFTY;  // TODO: Should we be multiplying this by OSQP_MIN_SCALING ?

    c_int n_ineq_l = 0;
    c_int n_ineq_u = 0;
    c_int n_eq = 0;
    c_int j;
    for (j = 0; j < m; j++) {
        c_float _l = l_data[j];
        c_float _u = u_data[j];
        if (_l < _u) {
            A_eq_vec[j] = 0;
            if (_l > -infval) {
                A_ineq_l_vec[j] = 1;
                n_ineq_l++;
            } else {
                A_ineq_l_vec[j] = 0;
            }
            if (_u < infval) {
                A_ineq_u_vec[j] = 1;
                n_ineq_u++;
            } else {
                A_ineq_u_vec[j] = 0;
            }
            nu_sign_vec[j] = 0;
        } else {
            A_eq_vec[j] = 1;
            A_ineq_l_vec[j] = 0;
            A_ineq_u_vec[j] = 0;
            n_eq++;
            if (y_data[j] >= 0) {
                nu_sign_vec[j] = 1;
            } else {
                nu_sign_vec[j] = -1;
            }
        }
    }

    OSQPVectori *A_ineq_l_i = OSQPVectori_new(A_ineq_l_vec, m);
    OSQPMatrix *A_ineq_l = OSQPMatrix_submatrix_byrows(A, A_ineq_l_i);
    OSQPMatrix_mult_scalar(A_ineq_l, -1);

    OSQPVectori *A_ineq_u_i = OSQPVectori_new(A_ineq_u_vec, m);
    OSQPMatrix *A_ineq_u = OSQPMatrix_submatrix_byrows(A, A_ineq_u_i);

    OSQPMatrix *G = OSQPMatrix_vstack(A_ineq_l, A_ineq_u);

    OSQPMatrix_free(A_ineq_l);
    OSQPMatrix_free(A_ineq_u);

    OSQPVectori *A_eq_i = OSQPVectori_new(A_eq_vec, m);
    OSQPMatrix *A_eq = OSQPMatrix_submatrix_byrows(A, A_eq_i);

    OSQPVectorf *zeros;

    // --------- lambda
    zeros = OSQPVectorf_malloc(m);
    OSQPVectorf_set_scalar(zeros, 0);

    OSQPVectorf *_y_l_ineq = OSQPVectorf_subvector_byrows(y, A_ineq_l_i);
    OSQPVectorf *y_l_ineq = OSQPVectorf_malloc(OSQPVectorf_length(_y_l_ineq));
    OSQPVectorf_ew_min_vec(y_l_ineq, _y_l_ineq, zeros);
    OSQPVectorf_free(_y_l_ineq);
    OSQPVectorf_mult_scalar(y_l_ineq, -1);

    OSQPVectorf *_y_u_ineq = OSQPVectorf_subvector_byrows(y, A_ineq_u_i);
    OSQPVectorf *y_u_ineq = OSQPVectorf_malloc(OSQPVectorf_length(_y_u_ineq));
    OSQPVectorf_ew_max_vec(y_u_ineq, _y_u_ineq, zeros);
    OSQPVectorf_free(_y_u_ineq);
    OSQPVectorf_free(zeros);

    OSQPVectorf *lambda = OSQPVectorf_concat(y_l_ineq, y_u_ineq);

    OSQPVectorf_free(y_l_ineq);
    OSQPVectorf_free(y_u_ineq);
    // ---------- lambda

    // --------- slacks
    OSQPVectorf *l_ineq = OSQPVectorf_subvector_byrows(l, A_ineq_l_i);
    OSQPVectorf_mult_scalar(l_ineq, -1);
    OSQPVectorf *u_ineq = OSQPVectorf_subvector_byrows(u, A_ineq_u_i);

    OSQPVectorf *h = OSQPVectorf_concat(l_ineq, u_ineq);

    OSQPVectorf_free(l_ineq);
    OSQPVectorf_free(u_ineq);

    OSQPVectorf* slacks = OSQPVectorf_copy_new(h);
    OSQPMatrix_Axpy(G, x, slacks, 1, -1);
    OSQPVectorf_free(h);

    // ---------- GDiagLambda
    OSQPMatrix *GDiagLambda = OSQPMatrix_copy_new(G);
    OSQPMatrix_lmult_diag(GDiagLambda, lambda);

    // ---------- P_full
    OSQPMatrix *P_full = OSQPMatrix_triu_to_symm(P);

    // ---------- RHS
    OSQPVectorf *dxx = OSQPVectorf_new(dx, n);
    OSQPVectorf *dy_l_vec = OSQPVectorf_new(dy_l, m);
    OSQPVectorf *dy_u_vec = OSQPVectorf_new(dy_u, m);

    OSQPVectorf *dy_l_ineq = OSQPVectorf_subvector_byrows(dy_l_vec, A_ineq_l_i);
    OSQPVectorf_free(dy_l_vec);
    OSQPVectorf *dy_u_ineq = OSQPVectorf_subvector_byrows(dy_u_vec, A_ineq_u_i);
    OSQPVectorf_free(dy_u_vec);
    OSQPVectorf *dlambd = OSQPVectorf_concat(dy_l_ineq, dy_u_ineq);
    OSQPVectorf_free(dy_l_ineq);
    OSQPVectorf_free(dy_u_ineq);

    c_float *d_nu_vec = (c_float *) c_malloc(n_eq * sizeof(c_float));
    for (j=0; j<n_eq; j++) {
        if (nu_sign_vec[j]==-1) {
            d_nu_vec[j] = dy_u[j];
        } else if (nu_sign_vec[j]==1) {
            d_nu_vec[j] = -dy_l[j];
        } else {}
    }
    OSQPVectorf *d_nu = OSQPVectorf_new(d_nu_vec, n_eq);
    c_free(d_nu_vec);

    OSQPVectorf *rhs_temp1 = OSQPVectorf_concat(dxx, dlambd);
    OSQPVectorf_free(dxx);
    OSQPVectorf_free(dlambd);
    OSQPVectorf *rhs_temp2 = OSQPVectorf_concat(rhs_temp1, d_nu);
    OSQPVectorf_free(rhs_temp1);
    OSQPVectorf_free(d_nu);
    OSQPVectorf_mult_scalar(rhs_temp2, -1);
    zeros = OSQPVectorf_malloc(n + n_ineq_l + n_ineq_u + n_eq);
    OSQPVectorf_set_scalar(zeros, 0);
    OSQPVectorf *rhs = OSQPVectorf_concat(rhs_temp2, zeros);
    OSQPVectorf_free(rhs_temp2);
    OSQPVectorf_free(zeros);

    // ----------- Check
    OSQPMatrix *checkmat1 = OSQPMatrix_new_from_csc(check1, 1);
    OSQPVectorf *checkvec2 = OSQPVectorf_new(check2, 2 * (n + n_ineq_l + n_ineq_u + n_eq));
    c_int status = adjoint_derivative_linsys_solver(solver, settings, P_full, G, A_eq, GDiagLambda, slacks, rhs, checkmat1, tol1);

    c_int status2 = OSQPVectorf_is_eq(rhs, checkvec2, tol2);
    status = status && status2;

    c_float *rhs_data = OSQPVectorf_data(rhs);

    c_float *r_yl = (c_float *) c_malloc(m * sizeof(c_float));
    c_float *r_yu = (c_float *) c_malloc(m * sizeof(c_float));

    for (j=0; j<m; j++) {
        if (A_eq_vec[j]) {
            if (y_data[j] >= 0) {
                r_yl[j] = 0;
                r_yu[j] = rhs_data[n + n_ineq_l + n_ineq_u + n_eq + n + n_ineq_l + n_ineq_u + j] / y_data[j];
            } else {
                r_yl[j] = -rhs_data[n + n_ineq_l + n_ineq_u + n_eq + n + n_ineq_l + n_ineq_u + j] / y_data[j];
                r_yu[j] = 0;
            }
        } else {
            if (A_ineq_l_vec[j]) {
                r_yl[j] = -rhs_data[n + n_ineq_l + n_ineq_u + n_eq + n + j - n_eq];
            } else {
                r_yl[j] = 0;
            }
            if (A_ineq_u_vec[j]) {
                r_yu[j] = rhs_data[n + n_ineq_l + n_ineq_u + n_eq + n + n_ineq_l + j - n_eq];
            } else {
                r_yu[j] = 0;
            }
        }
    }
    
    c_free(nu_sign_vec);

    OSQPMatrix_free(G);
    OSQPMatrix_free(A_eq);
    OSQPMatrix_free(P_full);
    OSQPMatrix_free(GDiagLambda);

    OSQPVectorf_free(lambda);
    OSQPVectorf_free(slacks);

    c_free(A_ineq_l_vec);
    c_free(A_ineq_u_vec);
    c_free(A_eq_vec);

    OSQPVectori_free(A_ineq_l_i);
    OSQPVectori_free(A_ineq_u_i);
    OSQPVectori_free(A_eq_i);

    OSQPVectorf_free(rhs);

    return status;
}