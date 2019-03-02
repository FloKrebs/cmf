#include "cvode3.h"
#include "../num_array.h"

#include <sstream>

#include <cvode/cvode.h>
#include <nvector/nvector_serial.h>
#include <sundials/sundials_types.h>   /* defs. of realtype, sunindextype      */
#include <sundials/sundials_version.h>


#include <cvode/cvode_direct.h>        /* access to CVDls interface            */

// Special includes for dense solver
#include <sunmatrix/sunmatrix_dense.h> /* access to dense SUNMatrix            */
#include <sunlinsol/sunlinsol_dense.h> /* access to dense SUNLinearSolver      */

// Special includes for banded solver
#include <sunmatrix/sunmatrix_band.h> /* access to band SUNMatrix            */
#include <sunlinsol/sunlinsol_band.h> /* access to band SUNLinearSolver      */

// Special includes for diagonal solver
#include <cvode/cvode_diag.h>

// Special includes for krylov preconditioned solver with spgmr linear solver
#include <cvode/cvode_spils.h>
#include <sunlinsol/sunlinsol_spgmr.h>
#include <cvode/cvode_bandpre.h>

#include <sunmatrix/sunmatrix_sparse.h>
#include <sunlinsol/sunlinsol_klu.h>

using namespace cmf::math;



cmf::math::CVodeOptions::CVodeOptions()
	: max_order(-1), max_non_linear_iterations(-1),
	max_error_test_failures(100000), max_convergence_failures(-1),
	max_num_steps(0), max_hnil_warnings(-1)
{

}


class cmf::math::CVode3::Impl {
public:
	/*******************************
	CVode Implentation Attributes
	********************************/
	// Set to false in CVodeAdams
	bool use_stiff_solver = true;
	// State vector
	N_Vector y = 0;
	// Jacobian matrix
	SUNMatrix J = 0;
	// Linear solver
	SUNLinearSolver LS = 0;
	// System size
	int N = 0;

	CVode3 * _integrator;

	// c'tor of CVode3::Impl, does not initialize solver
	Impl(CVode3 * integrator) :
		_integrator(integrator)
	{		}

	static int f(realtype t, N_Vector u, N_Vector udot, void *f_data)
	{
		CVode3::Impl * integ_impl = static_cast<CVode3::Impl*>(f_data);
		CVode3 * integ = integ_impl->_integrator;
		// Get the pointers to the data of the vectors u and udot
		realtype * udata = NV_DATA_S(u);
		realtype * dudata = NV_DATA_S(udot);
		// Get size of the problem
		int nsize = int(integ->size());
		// Update the states from the state vector
		try {
			integ->set_states(udata);
		}
		catch (std::exception& e) {
			std::cerr << e.what() << std::endl;
			integ->set_error_msg(e.what());
			return -8;
		}
		Time T = cmf::math::day * t;
		Time T_model = integ->get_t();
		real sec_local = (T - T_model) / sec;
		// Get the derivatives at time T
		try {
			integ->copy_dxdt(T, dudata);
		}
		catch (std::exception& e) {
			std::cerr << e.what() << std::endl;
			integ->set_error_msg(e.what());
			return -8;
		}
		return 0;
	}
	
	void * cvode_mem=0;
	
	void release()
	{
		// If saved vector exists, destroy it
		if (y != 0) N_VDestroy_Serial(y);
		if (cvode_mem != 0) CVodeFree(&cvode_mem);
		if (LS != 0) SUNLinSolFree(LS);
		if (J != 0) SUNMatDestroy(J);
	}
	
	int initialize() {
		release();
		// size of problem
		N = int(_integrator->m_States.size());
		// Allocate vector y
		y = N_VNew_Serial(N);
		// Pointer to data of vector y
		realtype * y_data = NV_DATA_S(y);
		// Copy states to y
		_integrator->copy_states(y_data);
		// Create a implicit (CV_BDF) solver with Newton iteration (CV_NEWTON)
		if (this->use_stiff_solver) {
			cvode_mem = CVodeCreate(CV_BDF);
		}
		else {
			cvode_mem = CVodeCreate(CV_ADAMS);
		}
		
		// Set this wrapper as user data of the solver
		CVodeSetUserData(cvode_mem, (void *)this);
		// Local copy of Epsilon needed for CVodeSVtolerances
		realtype reltol = _integrator->Epsilon;
		N_Vector abstol = N_VNew_Serial(N);
		realtype * abstol_data = NV_DATA_S(abstol);
		for (int i = 0; i < N; ++i)
			abstol_data[i] = _integrator->m_States[i]->get_abs_errtol(reltol*1e-3);
		int flag = 0;
		// Allocate memory for solver and set the right hand side function, start time and error tolerance
		flag = CVodeInit(cvode_mem, f, _integrator->get_t().AsDays(), y);
		flag = CVodeSVtolerances(cvode_mem, reltol, abstol);
		this->set_limits(_integrator->options);
		// Set solver
		this->_integrator->set_solver();

		
		return flag;
	}
	
	int reset() {
		if (cvode_mem) {
			// Copy current states to internal state vector
			_integrator->copy_states(NV_DATA_S(y));
			// Reinitialize solver
			return CVodeReInit(cvode_mem, get_t(), y);
		}
		else {
			return 0;
		}
	}

	void set_limits(const CVodeOptions& options) {

		if (options.max_order >= 0) CVodeSetMaxOrd(cvode_mem, options.max_order);
		// Set stability limit detection, see CVode Guide 2.3. Reasons to apply this algo are true for cmf.
		if (options.max_order >= 3) CVodeSetStabLimDet(cvode_mem, 1);
		if (options.max_non_linear_iterations >= 0) CVodeSetMaxNonlinIters(cvode_mem, options.max_non_linear_iterations);
		if (options.max_error_test_failures >= 0) CVodeSetMaxErrTestFails(cvode_mem, options.max_error_test_failures);
		if (options.max_convergence_failures >= 0) CVodeSetMaxConvFails(cvode_mem, options.max_convergence_failures);
		CVodeSetMaxNumSteps(cvode_mem, options.max_num_steps);
		CVodeSetMaxHnilWarns(cvode_mem, options.max_hnil_warnings);

	}

	realtype get_t() const {
		return _integrator->get_t().AsDays();
	}
	int integrate(cmf::math::Time t_max, cmf::math::Time dt) {
		if (_integrator->m_States.size() == 0)
			throw std::out_of_range("No states to integrate!");

		// If solver or y is not initialized, initialize them
		if (!cvode_mem || !y)
		{
			_integrator->m_dt = t_max - _integrator->get_t();
			this->initialize();
		}
		// Get data of y
		realtype * y_data = NV_DATA_S(y);
		// Time step, needed as return value
		realtype t_ret = get_t(),
				 t_step = 0;
		
		CVodeSetStopTime(cvode_mem, t_max.AsDays());
		_integrator->error_msg = "";
		int res = CVode(cvode_mem, t_max.AsDays(), y, &t_ret, CV_ONE_STEP);
		if (res<0) {
			_integrator->set_states(y_data);
			if (_integrator->error_msg != "")
				throw std::runtime_error(_integrator->error_msg + " - " + (day * t_ret).AsDate().to_string());
			else
				throw std::runtime_error("CVode could not integrate due to failure (see message above) at t=" + (day*t_ret).AsDate().to_string());
		}
		long iterations;
		CVodeGetNumRhsEvals(cvode_mem, &iterations);
		_integrator->m_Iterations = (int)iterations;
		CVodeGetLastStep(cvode_mem, &t_step);
		_integrator->m_dt = day * t_step;
		real sec_step = t_step * 24 * 60 * 60;
		double sec_to_go = (t_max - t_ret * day) / sec;
		if (res == CV_TSTOP_RETURN) {
			if ((day*t_ret - t_max).AsMilliseconds()>5 || (day*t_ret - t_max).AsMilliseconds()<-5) {
				throw std::runtime_error("Time returned by CVODE (" + (day*t_ret).to_string()
					+ ") differs from given end time("
					+ t_max.to_string() + "). Please inform cmf maintainer");
			}
			_integrator->m_t = t_max;
		}
		else {
			_integrator->m_t = t_ret * day;
		}


		// Copy result to state variables
		_integrator->set_states(y_data);
		return res;
	}

	cmf::math::num_array get_error() const {
		sunindextype N = _integrator->size();
		N_Vector ele = N_VNew_Serial(N);
		N_Vector eweight = N_VNew_Serial(N);
		CVodeGetEstLocalErrors(cvode_mem, ele);
		CVodeGetErrWeights(cvode_mem, eweight);
		N_Vector result = N_VNew_Serial(N);
		N_VProd(ele, eweight, result);
		num_array res(NV_DATA_S(result), NV_DATA_S(result) + N);
		N_VDestroy_Serial(ele);
		N_VDestroy_Serial(eweight);
		return res;
	}

	~Impl() {
		release();
	}
	static int sparse_jacobian(realtype t, N_Vector y, N_Vector fy, SUNMatrix J, void * userdata, N_Vector tmp1, N_Vector tmp2, N_Vector tmp3);
};


int cmf::math::CVode3::integrate(cmf::math::Time t_max, cmf::math::Time dt)
{

	return _implementation->integrate(t_max, dt);
}

void cmf::math::CVode3::reset()
{
	_implementation->reset();
}

cmf::math::CVode3::CVode3(cmf::math::StateVariableOwner & states, real epsilon)
	: Integrator(states, epsilon)
{
	cmf::math::CVode3::Impl* p_impl = new cmf::math::CVode3::Impl(this);
    _implementation=std::unique_ptr<cmf::math::CVode3::Impl>(p_impl);
}

void cmf::math::CVode3::set_error_msg(std::string error)
{
	_error_msg = error;
}

CVode3 * cmf::math::CVode3::copy() const
{
	return nullptr;
}
CVode3::~CVode3() = default;
CVodeInfo cmf::math::CVode3::get_info() const
{
	CVodeInfo ci;
	void * cvm = _implementation->cvode_mem;

	ci.size = size();
	CVodeGetWorkSpace(cvm, &ci.workspace_real, &ci.workspace_int);
	ci.workspace_byte = ci.workspace_real * sizeof(realtype) + ci.workspace_int * sizeof(long int);
	int qlast, qcur;
	realtype hinused, hlast, hcur, tcur;
	CVodeGetIntegratorStats(cvm, &ci.steps, &ci.rhs_evaluations, &ci.linear_solver_setups, &ci.error_test_fails,
		&qlast, &qcur, &hinused, &hlast, &hcur, &tcur);
	ci.current_order = qcur;
	CVodeGetNonlinSolvStats(cvm, &ci.nonlinear_solver_iterations, &ci.nonlinear_solver_convergence_failures);

	int version_len = 25;
	char version_buffer[25];
	SUNDIALSGetVersion(version_buffer, version_len);
	ci.sundials_version = std::string(version_buffer, version_len);

	return ci;
}

cmf::math::num_array cmf::math::CVode3::get_error() const
{
	return _implementation->get_error();
}

cmf::math::CVodeDense::CVodeDense(cmf::math::StateVariableOwner & states, real epsilon)
	: CVode3(states, epsilon)
{}

cmf::math::num_array cmf::math::CVodeDense::_get_jacobian() const
{
	CVode3::Impl& i = *_implementation;
	void * cvm = i.cvode_mem;

	SUNMatrix J = i.J;
	if (cvm == 0 || J == 0) {
		throw std::runtime_error(this->to_string() +  ": No access to Jacobian matrix, if the solver is not initialized. Run the solver for any timestep");
	}
	cmf::math::num_array res(SM_DATA_D(J), SM_DATA_D(J) + SM_LDATA_D(J));
	return res;
}

void cmf::math::CVodeDense::set_solver()
{
	CVode3::Impl& i = *_implementation;
	if (i.cvode_mem == 0) {
		throw std::runtime_error("Tried to create dense solver for uninitialized cvode");
	}
	i.J = SUNDenseMatrix(i.N, i.N);
	i.LS = SUNDenseLinearSolver(i.y, i.J);
	// Check for LS / J = NULL
	int flag = 0;
	flag = CVDlsSetLinearSolver(i.cvode_mem, i.LS, i.J);
	// Check flag and raise an error
	if (flag != CVLS_SUCCESS) {
		throw std::runtime_error("CVODE: Failed to create linear solver");
	}

}

cmf::math::CVodeBanded::CVodeBanded(cmf::math::StateVariableOwner & states, real epsilon, int _bandwidth)
	: CVode3(states, epsilon), bandwidth(_bandwidth)
{}

inline std::string cmf::math::CVodeBanded::to_string() const {
	return "CVodeBanded(w=" + std::to_string(bandwidth) + ")";

}

void cmf::math::CVodeBanded::set_solver()
{
	CVode3::Impl& i = *_implementation;
	if (i.cvode_mem == 0) {
		throw std::runtime_error("Tried to create banded solver for uninitialized cvode");
	}
	i.J = SUNBandMatrix(i.N, bandwidth, bandwidth);
	i.LS = SUNBandLinearSolver(i.y, i.J);
	int flag = CVDlsSetLinearSolver(i.cvode_mem, i.LS, i.J);
	// Check flag and raise an error
	if (flag != CVLS_SUCCESS) {
		throw std::runtime_error("CVODE: Failed to create linear solver");
	}

}

cmf::math::CVodeDiag::CVodeDiag(cmf::math::StateVariableOwner & states, real epsilon)
	: CVode3(states, epsilon)
{}

void cmf::math::CVodeDiag::set_solver()
{
	CVode3::Impl& i = *_implementation;
	if (i.cvode_mem == 0) {
		throw std::runtime_error("Tried to create solver for uninitialized cvode");
	}
	int flag = CVDiag(i.cvode_mem);

}

cmf::math::CVodeKrylov::CVodeKrylov(cmf::math::StateVariableOwner & states, real epsilon,
	int _bandwidth, char _preconditioner)
	: CVode3(states, epsilon), bandwidth(_bandwidth), preconditioner(_preconditioner)
{}

inline std::string cmf::math::CVodeKrylov::to_string() const {
	return "CVodeKrylov(w=" + std::to_string(bandwidth) + ", p='" + preconditioner + "')";
}

void cmf::math::CVodeKrylov::set_solver()
{
	CVode3::Impl& i = *_implementation;
	if (i.cvode_mem == 0) {
		throw std::runtime_error("Tried to create banded solver for uninitialized cvode");
	}
	int prec = PREC_LEFT;
	switch (preconditioner) {
	case 'L':
		prec = PREC_LEFT;
		break;
	case 'R':
		prec = PREC_RIGHT;
		break;
	case 'B':
		prec = PREC_BOTH;
		break;
	case 'N':
		prec = PREC_NONE;
		break;
	}
	i.LS = 0;
	i.LS = SUNSPGMR(i.y, prec, 0);
	if (i.LS == 0) {
		throw std::runtime_error("Linear solver not created");
	}
	int flag = CVodeSetLinearSolver(i.cvode_mem, i.LS, NULL);
	if (flag == CVLS_SUCCESS)
		flag = CVBandPrecInit(i.cvode_mem, i.N, bandwidth, bandwidth);
	else {
		throw std::runtime_error("Setting linear solver failed");
	}
}

std::string cmf::math::CVodeInfo::to_string() const
{
	std::ostringstream out;
	out << size << " state variables" << std::endl;
	out << "workspace (real/int/bytes): " << workspace_real << "/" << workspace_int << "/" << workspace_byte << std::endl;
	out << steps << " steps" << std::endl;
	out << "order is " << current_order << std::endl;
	out << rhs_evaluations << " rhs evaluations" << std::endl;
	out << linear_solver_setups << " linear solver setups" << std::endl;
	out << error_test_fails << " error test failures" << std::endl;
	out << nonlinear_solver_iterations << " non linear solver iterations" << std::endl;
	out << nonlinear_solver_convergence_failures << " non linear solver convergence failures" << std::endl;

	return out.str();
}

cmf::math::CVodeAdams::CVodeAdams(cmf::math::StateVariableOwner & states, real epsilon)
	:CVode3(states, epsilon)
{
	this->_implementation->use_stiff_solver = false;
}

std::string cmf::math::CVodeAdams::to_string() const
{
	return "CVodeAdams()";
}

cmf::math::CVodeKLU::CVodeKLU(cmf::math::StateVariableOwner & states, real epsilon)
	: CVode3(states, epsilon)
{
}

std::string cmf::math::CVodeKLU::to_string() const
{
	return "CVodeKLU()";
}

int klu_sparse_jacobian(
	realtype t,
	N_Vector y, N_Vector fy,
	SUNMatrix Jac,
	void *userdata,
	N_Vector tmp1, N_Vector tmp2, N_Vector tmp3
);



int CVode3::Impl::sparse_jacobian(
	realtype t,
	N_Vector y, N_Vector fy,
	SUNMatrix J,
	void *userdata,
	N_Vector tmp1, N_Vector tmp2, N_Vector tmp3
) {
	// Use lib/lib/cvode_ls.c:815 (cvLsDenseDQJac) as template

	/*
	Needs information about Jacobian structure already build in J. 
	Can be gained from J->indexvals and J->indexptrs
	*/
	CVode3::Impl * integ_impl = static_cast<CVode3::Impl*>(userdata);
	CVode3 * integ = integ_impl->_integrator;

	sunindextype NP = SM_NP_S(J); // Problem size
	sunindextype NNZ = SM_NNZ_S(J); // Number of Non-Zero values
	
	sunindextype 
		data_pos_start,  // Starting position in a row
		data_pos_end,    // End position in a row
		col;			 // current column number

	Time time = cmf::math::day * t;

	realtype inc = 1e-12; // TODO: Replace by complex definition from cvLsDenseDQJac
	realtype altered_dxdt = 0.0;
	realtype * J_data = SM_DATA_S(J);
	realtype * y_data = NV_DATA_S(y);
	realtype * f_data = NV_DATA_S(fy);

	realtype old_y = 0;
	for (int row = 0; row <= NP; ++row) {
		// Change state for the row
		old_y = integ->get_state(row);
		integ->set_state(row, old_y + inc);
		
		// Loop through column positions
		data_pos_start = SM_INDEXPTRS_S(J)[row];
		data_pos_end   = SM_INDEXPTRS_S(J)[row + 1];
		for (sunindextype data_pos = data_pos_start; data_pos < data_pos_end; ++data_pos) {
			// Get the column number
			col = SM_INDEXVALS_S(J)[data_pos];
			// Calculate the new dxdt for that column
			altered_dxdt = integ->m_States[col]->dxdt(time);
			// Set the Jacobian value
			J_data[data_pos] = altered_dxdt / inc - f_data[col] / inc;
		}

		// Undo state change
		integ->set_state(row, old_y);

	}
	return CVLS_SUCCESS;
}


void cmf::math::CVodeKLU::set_solver()
{
	throw std::logic_error("KLU solver not implemented yet");
	CVode3::Impl& i = *_implementation;
	int retval = 0;
	
	// Create sparse matrix

	// Number of non-zero entries, reduce to save memory
	int nnz = i.N * i.N; 
	i.J = SUNSparseMatrix(i.N, i.N, nnz, CSR_MAT);
	if (i.J == NULL) throw std::runtime_error("CVODE: Failed to construct sparse matrix");
	/* First population of J->indexvals and Jac->indexptrs from model structure */


	// Create Linear Solver and attach to CVode
	i.LS = SUNLinSol_KLU(i.y, i.J);
	if (i.LS == NULL) throw std::runtime_error("CVODE: Failed to construct sparse KLU linear solver");
	retval = CVodeSetLinearSolver(i.cvode_mem, i.LS, i.J);
	if (retval) throw std::runtime_error("CVODE: Failed to attach sparse KLU linear solver");

	/* Set the user-supplied Jacobian routine Jac */
	retval = CVodeSetJacFn(i.cvode_mem, klu_sparse_jacobian);
	if (retval) throw std::runtime_error("CVODE: Failed to set sparse jacobian function");

	
}
