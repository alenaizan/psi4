/*
 *  hf.cpp
 *  matrix
 *
 *  Created by Justin Turney on 4/9/08.
 *  Copyright 2008 by Justin M. Turney, Ph.D.. All rights reserved.
 *
 */

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>

#include <psifiles.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.h>
#include <libchkpt/chkpt.hpp>
#include <libparallel/parallel.h>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>
#include <psifiles.h>
#include "integralfunctors.h"
#include "pseudospectral.h"
#include "pkintegrals.h"
#include "df.h"

#include <libmints/mints.h>

#include "hf.h"

#ifdef _OPENMP
#include <omp.h>
#endif 

using namespace boost;
using namespace std;
using namespace psi;

namespace psi { namespace scf {

HF::HF(Options& options, shared_ptr<PSIO> psio, shared_ptr<Chkpt> chkpt)
    : Wavefunction(options, psio, chkpt),
      nuclear_dipole_contribution_(3),
      nuclear_quadrupole_contribution_(6),
      print_(1)
{
    common_init();
}

HF::HF(Options& options, shared_ptr<PSIO> psio)
    : Wavefunction(options, psio),
      nuclear_dipole_contribution_(3),
      nuclear_quadrupole_contribution_(6),
      print_(1)
{
    common_init();
}

HF::~HF()
{
}

void HF::common_init()
{
    integral_threshold_ = 0.0;
    //integral_threshold_ = 1.0E-14;

    scf_type_ = options_.get_str("SCF_TYPE");

    H_.reset(factory_->create_matrix("One-electron Hamiltonion"));
    S_.reset(factory_->create_matrix("S"));
    X_.reset(factory_->create_matrix("X"));
    //Sphalf_.reset(factory_->create_matrix("S^+1/2"));

    memset((void*) nsopi_, '\0', factory_->nirrep()*sizeof(int));
    memset((void*) nmopi_, '\0', factory_->nirrep()*sizeof(int));
    nmo_ = 0;
    nso_ = 0;
    nirrep_ = factory_->nirrep();
    int* dimpi = factory_->colspi();
    for (int h = 0; h< factory_->nirrep(); h++){
        nsopi_[h] = dimpi[h];
        nmopi_[h] = nsopi_[h]; //For now
        nso_ += nsopi_[h];
        nmo_ += nmopi_[h]; //For now 
    }

    // Form the SO lookup information
    so2symblk_ = new int[nso_];
    so2index_  = new int[nso_];
    size_t so_count = 0;
    size_t offset = 0;
    for (int h = 0; h < nirrep_; ++h) {
        for (int i = 0; i < nsopi_[h]; ++i) {
            so2symblk_[so_count] = h;
            so2index_[so_count] = so_count-offset;
            ++so_count;
        }
        offset += nsopi_[h];
    }


    Eold_    = 0.0;
    E_       = 0.0;
    maxiter_ = 40;

    // Read information from input file
    maxiter_ = options_.get_int("MAXITER");

    // Read in DOCC and SOCC from memory
    int nirreps = factory_->nirrep();
    int ndocc = 0, nsocc = 0;
    input_docc_ = false;
    if (options_["DOCC"].has_changed()) {
        input_docc_ = true;
        if(options_["DOCC"].size() != nirreps)
            throw PSIEXCEPTION("The DOCC array has the wrong dimensions");
        for (int i=0; i<nirreps; ++i) {
            doccpi_[i] = options_["DOCC"][i].to_integer();
            ndocc += 2*doccpi_[i];
        }
    } else {
        for (int i=0; i<nirreps; ++i)
            doccpi_[i] = 0;
    }

    input_socc_ = false;
    if (options_["SOCC"].has_changed()) {
        input_socc_ = true;
        if(options_["SOCC"].size() != nirreps)
            throw PSIEXCEPTION("The SOCC array has the wrong dimensions");
        for (int i=0; i<nirreps; ++i) {
            soccpi_[i] = options_["SOCC"][i].to_integer();
            nsocc += soccpi_[i];
        }
    } else {
        for (int i=0; i<nirreps; ++i)
            soccpi_[i] = 0;
    }

    // Read information from checkpoint
    nuclearrep_ = molecule_->nuclear_repulsion_energy();

    // Determine the number of electrons in the system
    charge_ = molecule_->molecular_charge();
    nelectron_  = 0;
    for (int i=0; i<molecule_->natom(); ++i)
        nelectron_ += (int)molecule_->Z(i);
    nelectron_ -= charge_;

    // If the user told us the multiplicity, read it from the input
    if(molecule_->multiplicity_specified()){
        multiplicity_ = molecule_->multiplicity();
    }else{
        if(nelectron_%2){
            multiplicity_ = 2;
            molecule_->set_multiplicity(2);
            // There are an odd number of electrons
            fprintf(outfile,"\tThere are an odd number of electrons - assuming doublet.\n"
                            "\tSpecify the multiplicity with the MULTP option in the\n"
                            "\tinput if this is incorrect\n\n");
        }else{
            multiplicity_ = 1;
            // There are an even number of electrons
            fprintf(outfile,"\tThere are an even number of electrons - assuming singlet.\n"
                            "\tSpecify the multiplicity with the MULTP option in the\n"
                            "\tinput if this is incorrect\n\n");
        }
    }

    // Make sure that the multiplicity is reasonable
    if(multiplicity_ - 1 > nelectron_){
        char *str = new char[100];
        sprintf(str, "There are not enough electrons for multiplicity = %d, \n"
                     "please check your input and use the MULTP keyword", multiplicity_);
        throw SanityCheckError(str, __FILE__, __LINE__);
        delete [] str;
    }
    if(multiplicity_%2 == nelectron_%2){
        char *str = new char[100];
        sprintf(str, "A multiplicity of %d with %d electrons is impossible.\n"
                     "Please check your input and use the MULTP and/or CHARGE keywords",
                     multiplicity_, nelectron_);
        throw SanityCheckError(str, __FILE__, __LINE__);
        delete [] str;
    }

    nbeta_  = (nelectron_ - multiplicity_ + 1)/2;
    nalpha_ = nbeta_ + multiplicity_ - 1;

    perturb_h_ = false;
    perturb_h_ = options_.get_bool("PERTURB_H");
    perturb_ = nothing;
    lambda_ = 0.0;
    if (perturb_h_) {
        string perturb_with;

        lambda_ = options_.get_double("LAMBDA");

        if (options_["PERTURB_WITH"].has_changed()) {
            perturb_with = options_.get_str("PERTURB_WITH");
            // Do checks to see what perturb_with is.
            if (perturb_with == "DIPOLE_X")
                perturb_ = dipole_x;
            else if (perturb_with == "DIPOLE_Y")
                perturb_ = dipole_y;
            else if (perturb_with == "DIPOLE_Z")
                perturb_ = dipole_z;
            else
                fprintf(outfile, "Unknown PERTURB_WITH. Applying no perturbation.\n");
        } else {
            fprintf(outfile, "PERTURB_H is true, but PERTURB_WITH not found, applying no perturbation.\n");
        }
    }

    // How much stuff shall we echo to the user?
    if(options_["PRINT"].has_changed())
        print_ = options_.get_int("PRINT");

    // Handle common diis info
    diis_enabled_ = true;
    min_diis_vectors_ = 4;

    // Allocate memory for DIISmin_diis_vectors_
    //  First, did the user request a different number of diis vectors?
    min_diis_vectors_ = options_.get_int("MIN_DIIS_VECTORS");
    max_diis_vectors_ = options_.get_int("MAX_DIIS_VECTORS");
    diis_start_ = options_.get_int("START_DIIS_ITER");
    diis_enabled_ = options_.get_bool("DIIS");

    // Don't perform DIIS if less than 2 vectors requested, or user requested a negative number
    if (min_diis_vectors_ < 2) {
        // disable diis
        diis_enabled_ = false;
    }

    initialized_diis_manager_ = false;

    print_header();
}
void HF::integrals()
{
    if (print_)
        fprintf(outfile, "  ==> Integral Setup <==\n\n");

    // We need some integrals on disk for these cases
    if (scf_type_ == "PK" || scf_type_ == "OUT_OF_CORE"){
        shared_ptr<MintsHelper> mints (new MintsHelper(options_, 0));
        mints->integrals();
        if(scf_type_ == "PK") pk_integrals_ = shared_ptr<PKIntegrals>(new PKIntegrals(memory_, psio_, options_, nirrep_,
                                                                                     nsopi_, so2index_, so2symblk_));
    }else if (scf_type_ == "PSEUDOSPECTRAL"){
        if(nirrep_ > 1)
            throw PSIEXCEPTION("SCF TYPE " + scf_type_ + " cannot use symmetry yet. Add 'symmetry c1' to the molecule specification");
        df_ = shared_ptr<DFHF>(new DFHF(basisset_, psio_, options_));
        pseudospectral_ = shared_ptr<PseudospectralHF>(new PseudospectralHF(basisset_, psio_, options_));
    }else if (scf_type_ == "DF"){
        df_ = shared_ptr<DFHF>(new DFHF(basisset_, psio_, options_));
    }else if (scf_type_ == "DIRECT"){
        if (print_)
            fprintf(outfile, "  Building Direct Integral Objects...\n\n");
        shared_ptr<IntegralFactory> integral = shared_ptr<IntegralFactory>(new IntegralFactory(basisset_, basisset_, basisset_, basisset_));
        shared_ptr<TwoBodyAOInt> aoeri = shared_ptr<TwoBodyAOInt>(integral->eri());
        eri_ = shared_ptr<TwoBodySOInt>(new TwoBodySOInt(aoeri, integral));
    }
}

void HF::finalize()
{
    delete[] so2symblk_;
    delete[] so2index_;

    pk_integrals_.reset();
    df_.reset();
    pseudospectral_.reset();
    eri_.reset();

    // Clean up after DIIS
    if(initialized_diis_manager_)
        diis_manager_->delete_diis_file();
    diis_manager_.reset();
    initialized_diis_manager_ = false;

    // Figure out how many frozen virtual and frozen core per irrep
    compute_fcpi();
    compute_fvpi();
    energy_ = E_;

    dump_to_checkpoint();

    S_.reset();
    //Sphalf_.reset();
    X_.reset();
    H_.reset();

    // Close the chkpt
    if(psio_->open_check(PSIF_CHKPT))
        psio_->close(PSIF_CHKPT, 1);
}

void HF::find_occupation()
{
    std::vector<std::pair<double, int> > pairs;
    for (int h=0; h<epsilon_a_->nirrep(); ++h) {
        for (int i=0; i<epsilon_a_->dimpi()[h]; ++i)
            pairs.push_back(make_pair(epsilon_a_->get(h, i), h));
    }
    sort(pairs.begin(),pairs.end());

    if(!input_docc_){
        memset(doccpi_, 0, sizeof(int) * epsilon_a_->nirrep());
        for (int i=0; i<nbeta_; ++i)
            doccpi_[pairs[i].second]++;
    }
    if(!input_socc_){
        memset(soccpi_, 0, sizeof(int) * epsilon_a_->nirrep());
        for (int i=nbeta_; i<nalpha_; ++i)
            soccpi_[pairs[i].second]++;
    }

    if(print_>5) {
        fprintf(outfile, "\tDOCC: [");
        for (int h=0; h<epsilon_a_->nirrep(); ++h){
            fprintf(outfile, "%3d ", doccpi_[h]);
        }
        fprintf(outfile, "]\n");
        fprintf(outfile, "\tSOCC: [");
        for (int h=0; h<epsilon_a_->nirrep(); ++h){
            fprintf(outfile, "%3d ", soccpi_[h]);
        }
        fprintf(outfile, "]\n");
    }

    for (int i=0; i<epsilon_a_->nirrep(); ++i) {
        nalphapi_[i] = doccpi_[i] + soccpi_[i];
        nbetapi_[i]  = doccpi_[i];
    }
}

void HF::print_header()
{
    int nthread = 1;
    #ifdef _OPENMP
        nthread = omp_get_max_threads();
    #endif 

    fprintf(outfile, "\n");
    fprintf(outfile, "         ---------------------------------------------------------\n");
    fprintf(outfile, "                                   SCF\n"); 
    fprintf(outfile, "            by Justin Turney, Rob Parrish, and Andy Simmonnett\n");
    fprintf(outfile, "                             %4s Reference\n", options_.get_str("REFERENCE").c_str());
    fprintf(outfile, "                      %3d Threads, %6ld MiB Core\n", nthread, memory_ / 1000000L);
    fprintf(outfile, "         ---------------------------------------------------------\n");
    fprintf(outfile, "\n");

    fprintf(outfile, "  ==> Geometry <==\n\n");

    molecule_->print();

    fprintf(outfile, "  Running in %s symmetry.\n\n", molecule_->point_group()->symbol());

    fprintf(outfile, "  Nuclear repulsion = %20.15f\n\n", nuclearrep_);
    fprintf(outfile, "  Charge       = %d\n", charge_);
    fprintf(outfile, "  Multiplicity = %d\n", multiplicity_);
    fprintf(outfile, "  Electrons    = %d\n", nelectron_);
    fprintf(outfile, "  Nalpha       = %d\n", nalpha_);
    fprintf(outfile, "  Nbeta        = %d\n\n", nbeta_);

    fprintf(outfile, "  ==> Algorithm <==\n\n");
    fprintf(outfile, "  SCF Algorithm Type is %s.\n", options_.get_str("SCF_TYPE").c_str());
    fprintf(outfile, "  DIIS %s.\n", diis_enabled_ ? "enabled" : "disabled");
    fprintf(outfile, "  Guess Type is %s.\n", options_.get_str("GUESS").c_str());
    fprintf(outfile, "  Energy threshold   = %3.2e\n", energy_threshold_);
    fprintf(outfile, "  Density threshold  = %3.2e\n", density_threshold_);
    fprintf(outfile, "  Integral threshold = %3.2e\n\n", integral_threshold_);
    fflush(outfile);

    fprintf(outfile, "  ==> Primary Basis: %s <==\n\n", options_.get_str("BASIS").c_str());
    basisset_->print_by_level(outfile, print_); 

}
void HF::print_preiterations()
{
    CharacterTable ct = molecule_->point_group()->char_table();

    fprintf(outfile, "   -------------------------------------------------------\n");
    fprintf(outfile, "    Irrep   Nso     Nmo     Nalpha   Nbeta   Ndocc  Nsocc\n");
    fprintf(outfile, "   -------------------------------------------------------\n");
    for (int h= 0; h < nirrep_; h++) {
        fprintf(outfile, "     %-3s   %6d  %6d  %6d  %6d  %6d  %6d\n", ct.gamma(h).symbol(), nsopi_[h], nmopi_[h], nalphapi_[h], nbetapi_[h], doccpi_[h], soccpi_[h]);   
    }
    fprintf(outfile, "   -------------------------------------------------------\n");
    fprintf(outfile, "    Total  %6d  %6d  %6d  %6d  %6d  %6d\n", nso_, nmo_, nalpha_, nbeta_, nbeta_, nalpha_-nbeta_);   
    fprintf(outfile, "   -------------------------------------------------------\n\n");
}

void HF::form_H()
{
    SharedMatrix kinetic(factory_->create_matrix("Kinetic Integrals"));
    SharedMatrix potential(factory_->create_matrix("Potential Integrals"));

    // Load in kinetic and potential matrices
    double *integrals = init_array(ioff[nso_]);

    // Integral factory
    shared_ptr<IntegralFactory> integral(new IntegralFactory(basisset_, basisset_, basisset_, basisset_));
    shared_ptr<OneBodySOInt>    soT(integral->so_kinetic());
    shared_ptr<OneBodySOInt>    soV(integral->so_potential());

    soT->compute(kinetic);
    soV->compute(potential);

    if (debug_ > 2)
        kinetic->print(outfile);

    if (debug_ > 2)
        potential->print(outfile);

    H_->copy(kinetic);
    H_->add(potential);

    if (debug_ > 2)
        H_->print(outfile);

    free(integrals);

    if (perturb_h_) {
        shared_ptr<IntegralFactory> ifact(new IntegralFactory(basisset_, basisset_, basisset_, basisset_));
        MultipoleSymmetry msymm(1, molecule_, ifact, factory_);
        vector<SharedMatrix> dipoles = msymm.create_matrices("Dipole");

        OneBodySOInt *so_dipole = ifact->so_dipole();
        so_dipole->compute(dipoles);

        if (perturb_ == dipole_x) {
            if (msymm.component_symmetry(0) != 0){
                fprintf(outfile, "  WARNING: You requested mu(x) perturbation, but mu(x) is not symmetric.\n");
            }
            else {
                fprintf(outfile, "  Perturbing H by %f mu(x).\n", lambda_);
                dipoles[0]->scale(lambda_);
                H_->add(dipoles[0]);
            }
        } else if (perturb_ == dipole_y) {
            if (msymm.component_symmetry(1) != 0){
                fprintf(outfile, "  WARNING: You requested mu(y) perturbation, but mu(y) is not symmetric.\n");
            }
            else {
                fprintf(outfile, "  Perturbing H by %f mu(y).\n", lambda_);
                dipoles[1]->scale(lambda_);
                H_->add(dipoles[1]);
            }
        } else if (perturb_ == dipole_z) {
            if (msymm.component_symmetry(2) != 0){
                fprintf(outfile, "  WARNING: You requested mu(z) perturbation, but mu(z) is not symmetric.\n");
            }
            else {
                fprintf(outfile, "  Perturbing H by %f mu(z).\n", lambda_);
                dipoles[2]->scale(lambda_);
                H_->add(dipoles[2]);
            }
        }
    }

    if (print_ > 3)
        H_->print(outfile);
}

void HF::form_Shalf()
{
    // ==> SYMMETRIC ORTHOGONALIZATION <== //

    shared_ptr<IntegralFactory> integral(new IntegralFactory(basisset_, basisset_, basisset_, basisset_));
    shared_ptr<OneBodySOInt>   so_overlap(integral->so_overlap());
    so_overlap->compute(S_);

    SharedMatrix eigvec= factory_->create_shared_matrix("L");
    SharedMatrix eigtemp= factory_->create_shared_matrix("Temp");
    SharedMatrix eigtemp2= factory_->create_shared_matrix("Temp2");
    SharedMatrix eigvec_store= factory_->create_shared_matrix("TempStore");
    SharedVector eigval(factory_->create_vector());
    SharedVector eigval_store(factory_->create_vector());

    //Used to do this 3 times, now only once
    S_->diagonalize(eigvec, eigval);
    eigvec_store->copy(eigvec);
    eigval_store->copy(eigval.get());

    // Convert the eigenvales to 1/sqrt(eigenvalues)
    int *dimpi = eigval->dimpi();
    double min_S = fabs(eigval->get(0,0));
    for (int h=0; h<eigval->nirrep(); ++h) {
        for (int i=0; i<dimpi[h]; ++i) {
            if (min_S > eigval->get(h,i))
                min_S = eigval->get(h,i);
            double scale = 1.0 / sqrt(eigval->get(h, i));
            eigval->set(h, i, scale);
        }
    }
    if (print_)
        fprintf(outfile,"  Minimum eigenvalue in the overlap matrix is %14.10E.\n",min_S);
    // Create a vector matrix from the converted eigenvalues
    eigtemp2->set_diagonal(eigval);

    eigtemp->gemm(false, true, 1.0, eigtemp2, eigvec, 0.0);
    X_->gemm(false, false, 1.0, eigvec, eigtemp, 0.0);

    // ==> CANONICAL ORTHOGONALIZATION <== //

    // Decide symmetric or canonical 
    double S_cutoff = options_.get_double("S_MIN_EIGENVALUE");
    if (min_S > S_cutoff && options_.get_str("S_ORTHOGONALIZATION") == "SYMMETRIC") {

        if (print_)
            fprintf(outfile,"  Using Symmetric Orthogonalization.\n");

    } else {

        if (print_)
            fprintf(outfile,"  Using Canonical Orthogonalization with cutoff of %14.10E.\n",S_cutoff);

        //Diagonalize S (or just get a fresh copy)
        eigvec->copy(eigvec_store.get());
        eigval->copy(eigval_store.get());
        int delta_mos = 0;
        for (int h=0; h<eigval->nirrep(); ++h) {
            //in each irrep, scale significant cols i  by 1.0/sqrt(s_i)
            int start_index = 0;
            for (int i=0; i<dimpi[h]; ++i) {
                if (S_cutoff  < eigval->get(h,i)) {
                    double scale = 1.0 / sqrt(eigval->get(h, i));
                    eigvec->scale_column(h, i, scale);
                } else {
                    start_index++;
                    nmopi_[h]--;
                    nmo_--;
                }
            }
            if (print_>2)
                fprintf(outfile,"  Irrep %d, %d of %d possible MOs eliminated.\n",h,start_index,nsopi_[h]);

            delta_mos += start_index;
        }

        X_->init(eigvec->nirrep(),nsopi_,nmopi_,"X (Canonical Orthogonalization)");
        for (int h=0; h<eigval->nirrep(); ++h) {
            //Copy significant columns of eigvec into X in
            //descending order
            int start_index = 0;
            for (int i=0; i<dimpi[h]; ++i) {
                if (S_cutoff  < eigval->get(h,i)) {
                } else {
                    start_index++;    
                }
            }
            for (int i=0; i<dimpi[h]-start_index; ++i) {
                for (int m = 0; m < dimpi[h]; m++)
                    X_->set(h,m,i,eigvec->get(h,m,dimpi[h]-i-1));
            }
        }

        if (print_)
            fprintf(outfile,"  Overall, %d of %d possible MOs eliminated.\n",delta_mos,nso_);

        // Refreshes twice in RHF, no big deal
        epsilon_a_->init(eigvec->nirrep(), nmopi_);
        Ca_->init(eigvec->nirrep(),nsopi_,nmopi_,"MO coefficients");
        epsilon_b_->init(eigvec->nirrep(), nmopi_);
        Cb_->init(eigvec->nirrep(),nsopi_,nmopi_,"MO coefficients");
    }

    if (print_ > 3) {
        S_->print(outfile);
        X_->print(outfile);
    }
    if (print_) {
        fprintf(outfile,"\n");
        fflush(outfile);
    }
}

void HF::compute_fcpi()
{
    int nfzc = molecule_->nfrozen_core();
    // Print out orbital energies.
    std::vector<std::pair<double, int> > pairs;
    for (int h=0; h<epsilon_a_->nirrep(); ++h) {
        for (int i=0; i<epsilon_a_->dimpi()[h]; ++i)
            pairs.push_back(make_pair(epsilon_a_->get(h, i), h));
        frzcpi_[h] = 0;
    }
    sort(pairs.begin(),pairs.end());

    for (int i=0; i<nfzc; ++i)
        frzcpi_[pairs[i].second]++;
}

void HF::compute_fvpi()
{
    int nfzv = options_.get_int("FREEZE_VIRT");
    // Print out orbital energies.
    std::vector<std::pair<double, int> > pairs;
    for (int h=0; h<epsilon_a_->nirrep(); ++h) {
        for (int i=0; i<epsilon_a_->dimpi()[h]; ++i)
            pairs.push_back(make_pair(epsilon_a_->get(h, i), h));
        frzvpi_[h] = 0;
    }
    sort(pairs.begin(),pairs.end(), greater<std::pair<double, int> >());

    for (int i=0; i<nfzv; ++i)
        frzvpi_[pairs[i].second]++;
}

void HF::print_orbitals(const char* header, int *&irrep_count,
                        const std::vector< std::pair<double, int> >& evals,
                        int start, int end)
{
    char **labels = molecule_->irrep_labels();
    fprintf(outfile, "\t%-70s\n\n\t", header);
    int count = 0;
    for (int i = start; i < end; ++i) {
        int irrep = evals[i].second;
        fprintf(outfile, "%4d%-4s%10.6f  ", ++irrep_count[irrep], labels[irrep], evals[i].first);
        if (count++ % 3 == 2 && count != end)
            fprintf(outfile, "\n\t");
    }
    fprintf(outfile, "\n\n");
    for(int h = 0; h < nirrep_; ++h)
        delete [] labels[h];
}

void HF::guess()
{
    //What does the user want?
    //Options will be:
    // "READ"-try to read MOs from file100, projecting if needed 
    // "CORE"-CORE Hamiltonain
    // "GWH"-Generalized Wolfsberg-Helmholtz
    // "SAD"-Superposition of Atomic Denisties
    string guess_type = options_.get_str("GUESS");
    if (guess_type == "READ") {

        if (print_)
            fprintf(outfile, "  SCF Guess: Projection.\n\n");

        load_orbitals();
        form_D();

    } else if (guess_type == "SAD") {

        if (print_)
            fprintf(outfile, "  SCF Guess: Superposition of Atomic Densities via on-the-fly atomic UHF.\n\n");

        //Superposition of Atomic Density (RHF only at present)
        compute_SAD_guess();

    } else if (guess_type == "GWH") {
        //Generalized Wolfsberg Helmholtz (Sounds cool, easy to code)
        if (print_)
            fprintf(outfile, "  SCF Guess: Generalized Wolfsberg-Helmholtz.\n\n");

        Fa_->zero(); //Try Fa_{mn} = S_{mn} (H_{mm} + H_{nn})/2
        int h, i, j;
        int *opi = S_->rowspi();
        int nirreps = S_->nirrep();
        for (h=0; h<nirreps; ++h) {
            for (i=0; i<opi[h]; ++i) {
                for (j=0; j<opi[h]; ++j) {
                    Fa_->set(h,i,j,0.5*S_->get(h,i,j)*(H_->get(h,i,i)+H_->get(h,j,j)));
                }
            }
        }
        Fb_->copy(Fa_);

        form_initial_C();
        find_occupation();
        form_D();
        E_ = compute_initial_E();

    } else if (guess_type == "CORE") {

        if (print_)
            fprintf(outfile, "  SCF Guess: Core (One-Electron) Hamiltonian.\n\n");

        Fa_->copy(H_); //Try the core Hamiltonian as the Fock Matrix
        Fb_->copy(H_);

        form_initial_C();
        find_occupation();
        form_D();
        E_ = compute_initial_E();
    }
    if (print_ > 3) {
        Ca_->print();
        Cb_->print();
        Da_->print();
        Db_->print();
        Fa_->print();
        Fb_->print();
    }
    
    if (print_) 
        fprintf(outfile, "  Initial %s energy: %20.14f\n\n", options_.get_str("REFERENCE").c_str(), E_);
}
void HF::save_orbitals()
{
    psio_->open(PSIF_SCF_DB_MOS,PSIO_OPEN_NEW);

    if (print_)
        fprintf(outfile,"\n  Saving occupied orbitals to File 100.\n");

    psio_->write_entry(PSIF_SCF_DB_MOS,"DB SCF ENERGY",(char *) &(E_),sizeof(double));
    psio_->write_entry(PSIF_SCF_DB_MOS,"DB NIRREP",(char *) &(nirrep_),sizeof(int));
    psio_->write_entry(PSIF_SCF_DB_MOS,"DB NSOPI",(char *) &(nsopi_),8*sizeof(int));
    psio_->write_entry(PSIF_SCF_DB_MOS,"DB NALPHAPI",(char *) (nalphapi_),8*sizeof(int));
    psio_->write_entry(PSIF_SCF_DB_MOS,"DB NBETAPI",(char *) (nbetapi_),8*sizeof(int));

    char *basisname = strdup(options_.get_str("BASIS").c_str());
    int basislength = strlen(options_.get_str("BASIS").c_str()) + 1;

    psio_->write_entry(PSIF_SCF_DB_MOS,"DB BASIS NAME LENGTH",(char *)(&basislength),sizeof(int)); 
    psio_->write_entry(PSIF_SCF_DB_MOS,"DB BASIS NAME",basisname,basislength*sizeof(char)); 
    
    shared_ptr<Matrix> Ctemp_a(new Matrix("DB ALPHA MOS", nirrep_, nsopi_, nalphapi_));
    for (int h = 0; h < nirrep_; h++)
        for (int m = 0; m<nsopi_[h]; m++)
            for (int i = 0; i<nalphapi_[h]; i++)
                Ctemp_a->set(h,m,i,Ca_->get(h,m,i));
    Ctemp_a->save(psio_, PSIF_SCF_DB_MOS, Matrix::SubBlocks);

    shared_ptr<Matrix> Ctemp_b(new Matrix("DB BETA MOS", nirrep_, nsopi_, nbetapi_));
    for (int h = 0; h < nirrep_; h++)
        for (int m = 0; m<nsopi_[h]; m++)
            for (int i = 0; i<nbetapi_[h]; i++)
                Ctemp_b->set(h,m,i,Cb_->get(h,m,i));
    Ctemp_b->save(psio_, PSIF_SCF_DB_MOS, Matrix::SubBlocks);

    psio_->close(PSIF_SCF_DB_MOS,1);
}
void HF::load_orbitals()
{
    psio_->open(PSIF_SCF_DB_MOS,PSIO_OPEN_OLD);

    int basislength;
    psio_->read_entry(PSIF_SCF_DB_MOS,"DB BASIS NAME LENGTH",(char *)(&basislength),sizeof(int)); 
    char basisnamec[basislength];
    psio_->read_entry(PSIF_SCF_DB_MOS,"DB BASIS NAME",basisnamec,basislength*sizeof(char)); 

    std::string basisname(basisnamec);   

    if (basisname == "")
        throw PSIEXCEPTION("SCF::load_orbitals: Custom basis sets not allowed for small-basis guess");
 
    if (print_) {
        if (basisname != options_.get_str("BASIS")) {
            fprintf(outfile,"  Computing dual basis set projection from %s to %s.\n", \
                basisname.c_str(),options_.get_str("BASIS").c_str());
        } else {
            fprintf(outfile,"  Using orbitals from previous SCF, no projection.\n");
        }
    }

    shared_ptr<BasisSetParser> parser(new Gaussian94BasisSetParser());
    molecule_->set_basis_all_atoms(basisname, "DUAL_BASIS_SCF");
    shared_ptr<BasisSet> dual_basis = BasisSet::construct(parser, molecule_, "DUAL_BASIS_SCF");

    psio_->read_entry(PSIF_SCF_DB_MOS,"DB SCF ENERGY",(char *) &(E_),sizeof(double));

    int old_nirrep, old_nsopi[8];
    psio_->read_entry(PSIF_SCF_DB_MOS,"DB NIRREP",(char *) &(old_nirrep),sizeof(int));

    if (old_nirrep != nirrep_) 
        throw PSIEXCEPTION("SCF::load_orbitals: Projection of orbitals between different symmetries is not currently supported");

    psio_->read_entry(PSIF_SCF_DB_MOS,"DB NSOPI",(char *) (old_nsopi),8*sizeof(int));
    psio_->read_entry(PSIF_SCF_DB_MOS,"DB NALPHAPI",(char *) (nalphapi_),8*sizeof(int));
    psio_->read_entry(PSIF_SCF_DB_MOS,"DB NBETAPI",(char *) (nbetapi_),8*sizeof(int));

    for (int h = 0; h < nirrep_; h++) {
        doccpi_[h] = nbetapi_[h];
        soccpi_[h] = nalphapi_[h] - nbetapi_[h];
    }

    shared_ptr<Matrix> Ctemp_a(new Matrix("DB ALPHA MOS", nirrep_, old_nsopi, nalphapi_));
    Ctemp_a->load(psio_, PSIF_SCF_DB_MOS, Matrix::SubBlocks);
    shared_ptr<Matrix> Ca;
    if (basisname != options_.get_str("BASIS")) {
        Ca = dualBasisProjection(Ctemp_a, nalphapi_, dual_basis, basisset_);
    } else {
        Ca = Ctemp_a;
    }
    for (int h = 0; h < nirrep_; h++)
        for (int m = 0; m<nsopi_[h]; m++)
            for (int i = 0; i<nalphapi_[h]; i++)
                Ca_->set(h,m,i,Ca->get(h,m,i));

    shared_ptr<Matrix> Ctemp_b(new Matrix("DB BETA MOS", nirrep_, old_nsopi, nbetapi_));
    Ctemp_b->load(psio_, PSIF_SCF_DB_MOS, Matrix::SubBlocks);
    shared_ptr<Matrix> Cb;
    if (basisname != options_.get_str("BASIS")) {
        Cb = dualBasisProjection(Ctemp_b, nbetapi_, dual_basis, basisset_);
    } else {
        Cb = Ctemp_b;
    }
    for (int h = 0; h < nirrep_; h++)
        for (int m = 0; m<nsopi_[h]; m++)
            for (int i = 0; i<nbetapi_[h]; i++)
                Cb_->set(h,m,i,Cb->get(h,m,i));

    psio_->close(PSIF_SCF_DB_MOS,1);
}

void HF::dump_to_checkpoint()
{
    if(!psio_->open_check(PSIF_CHKPT))
        psio_->open(PSIF_CHKPT, PSIO_OPEN_OLD);
    chkpt_->wt_nirreps(nirrep_);
    char **labels = molecule_->irrep_labels();
    chkpt_->wt_irr_labs(labels);
    for(int h = 0; h < nirrep_; ++h)
        delete [] labels[h];
    delete [] labels;
    chkpt_->wt_nmo(nmo_);
    chkpt_->wt_nso(nso_);
    chkpt_->wt_nao(basisset_->nao());
    chkpt_->wt_ref(0);
    chkpt_->wt_etot(E_);
    chkpt_->wt_escf(E_);
    chkpt_->wt_eref(E_);
    chkpt_->wt_enuc(molecule_->nuclear_repulsion_energy());
    chkpt_->wt_orbspi(nmopi_);
    chkpt_->wt_clsdpi(doccpi_);
    chkpt_->wt_openpi(soccpi_);
    chkpt_->wt_phase_check(0);
    chkpt_->wt_sopi(nsopi_);
    // Figure out frozen core orbitals
    int nfzc = molecule_->nfrozen_core();
    int nfzv = options_.get_int("FREEZE_VIRT");
    chkpt_->wt_nfzc(nfzc);
    chkpt_->wt_nfzv(nfzv);
    // These were computed by HF::finalize()
    chkpt_->wt_frzcpi(frzcpi_);
    chkpt_->wt_frzvpi(frzvpi_);

    int m = 0;
    for(int h = 0; h < nirrep_; ++h)
        if(soccpi_[h]) ++m;
    chkpt_->wt_iopen(m*(m+1)/2);

    if(options_.get_str("REFERENCE") == "UHF"){
        double* values = epsilon_a_->to_block_vector();
        chkpt_->wt_alpha_evals(values);
        free(values);
        values = epsilon_b_->to_block_vector();
        chkpt_->wt_beta_evals(values);
        free(values);
        double** vectors = Ca_->to_block_matrix();
        chkpt_->wt_alpha_scf(vectors);
        free_block(vectors);
        vectors = Cb_->to_block_matrix();
        chkpt_->wt_beta_scf(vectors);
        free_block(vectors);
    }else{
        // All other reference type yield restricted orbitals
        double* values = epsilon_a_->to_block_vector();
        chkpt_->wt_evals(values);
        free(values);
        double** vectors = Ca_->to_block_matrix();
        chkpt_->wt_scf(vectors);
        free_block(vectors);
        double *ftmp = Fa_->to_lower_triangle();
        chkpt_->wt_fock(ftmp);
        delete[] ftmp;
    }
    psio_->close(PSIF_CHKPT, 1);
}

double HF::compute_energy()
{
    std::string reference = options_.get_str("REFERENCE");

    bool converged = false, diis_iter = false;

    // Neither of these are idempotent
    if ((options_.get_str("GUESS") == "SAD") || (options_.get_str("GUESS") == "READ"))
        iteration_ = -1;
    else
        iteration_ = 0;

    if (print_)
        fprintf(outfile, "  ==> Pre-Iterations <==\n\n");

    timer_on("Form H");
    form_H(); //Core Hamiltonian
    timer_off("Form H");

    timer_on("Form S/X");
    form_Shalf(); //S and X Matrix
    timer_off("Form S/X");

    timer_on("Guess");
    guess(); // Guess
    timer_off("Guess");

    if (print_)
        print_preiterations();

    integrals();     

    fprintf(outfile, "  ==> Iterations <==\n\n");
    fprintf(outfile, "                        Total Energy        Delta E      Density RMS\n\n");
    fflush(outfile);

    // SCF iterations
    do {
        iteration_++;

        save_density_and_energy();

        // Call any preiteration callbacks
        call_preiteration_callbacks();

        timer_on("Form G");
        form_G();
        timer_off("Form G");

        // Reset fractional SAD occupation
        if (iteration_ == 0 && options_.get_str("GUESS") == "SAD")
            reset_SAD_occupation();

        timer_on("Form F");
        form_F();
        timer_off("Form F");

        if (print_>3) {
            Fa_->print(outfile);
            Fb_->print(outfile);
        }

        E_ = compute_E();

        timer_on("DIIS");
        if (diis_enabled_ && iteration_ > 0 && iteration_ >= diis_start_ )
            save_fock();
        if (diis_enabled_ == true && iteration_ >= diis_start_ + min_diis_vectors_ - 1) {
            diis_iter = diis();
        } else {
            diis_iter = false;
        }
        timer_off("DIIS");

        if (print_>4 && diis_iter) {
            fprintf(outfile,"  After DIIS:\n");
            Fa_->print(outfile);
            Fb_->print(outfile);
        }

        fprintf(outfile, "   @%s iter %3d: %20.14f   % 10.5e   % 10.5e %s\n", reference.c_str(), iteration_, E_, E_ - Eold_, Drms_, diis_iter == false ? " " : "DIIS");
        fflush(outfile);

        timer_on("Form C");
        form_C();
        timer_off("Form C");
        timer_on("Form D");
        form_D();
        timer_off("Form D");

        if (print_ > 3){
            Ca_->print(outfile);
            Cb_->print(outfile);
            Da_->print(outfile);
            Db_->print(outfile);
        }

        converged = test_convergency();

        // Call any postiteration callbacks
        call_postiteration_callbacks();

    } while (!converged && iteration_ < maxiter_ );

    if (print_) 
        fprintf(outfile, "\n  ==> Post-Iterations <==\n\n");

    if (converged) {
        // Need to recompute the Fock matrices, as they are modified during the SCF interation
        // and might need to be dumped to checkpoint later
        form_F();

        // Print the orbitals
        if(print_){
            fprintf(outfile, "\tOrbital Energies (a.u.)\n\t-----------------------\n\n");
            std::vector<std::pair<double, int> > aPairs;
            std::vector<std::pair<double, int> > bPairs;
            for (int h = 0; h < nirrep_; ++h) {
                for (int i=0; i < nmopi_[h]; ++i){
                    aPairs.push_back(make_pair(epsilon_a_->get(h, i), h));
                    bPairs.push_back(make_pair(epsilon_b_->get(h, i), h));
                }
            }
            sort(aPairs.begin(), aPairs.end());
            sort(bPairs.begin(), bPairs.end());
            int *irrep_count = new int[nirrep_];
            ::memset(irrep_count, 0, nirrep_ * sizeof(int));
            if((reference == "RHF") || (reference == "RKS")){
                print_orbitals("Doubly Occupied:", irrep_count, aPairs, 0, nalpha_);
                print_orbitals("Virtual:", irrep_count, aPairs, nalpha_, nmo_);
            }else if((reference == "UHF") || (reference == "UKS")){
                print_orbitals("Alpha Occupied:", irrep_count, aPairs, 0, nalpha_);
                print_orbitals("Alpha Virtual:", irrep_count, aPairs, nalpha_, nmo_);
                ::memset(irrep_count, 0, nirrep_ * sizeof(int));
                print_orbitals("Beta Occupied:", irrep_count, bPairs, 0, nbeta_);
                print_orbitals("Beta Virtual:", irrep_count, bPairs, nbeta_, nmo_);
            }else if(reference == "ROHF"){
                print_orbitals("Doubly Occupied:", irrep_count, aPairs, 0, nbeta_);
                print_orbitals("Singly Occupied:", irrep_count, aPairs, nbeta_, nalpha_);
                print_orbitals("Virtual:", irrep_count, aPairs, nalpha_, nmo_);
            }else{
                throw PSIEXCEPTION("Unknown reference in HF::print_orbitals");
            }

            char **labels = molecule_->irrep_labels();
            fprintf(outfile, "\tFinal Occupation by Irrep:\n");
            fprintf(outfile, "\t      ");
            for(int h = 0; h < nirrep_; ++h) fprintf(outfile, " %4s ", labels[h]); fprintf(outfile, "\n");
            fprintf(outfile, "\tDOCC [ ");
            for(int h = 0; h < nirrep_-1; ++h) fprintf(outfile, " %4d,", doccpi_[h]);
            fprintf(outfile, " %4d ]\n", doccpi_[nirrep_-1]);
            if(reference != "RHF"){
                fprintf(outfile, "\tSOCC [ ");
                for(int h = 0; h < nirrep_-1; ++h) fprintf(outfile, " %4d,", soccpi_[h]);
                fprintf(outfile, " %4d ]\n", soccpi_[nirrep_-1]);
            }
            for(int h = 0; h < nirrep_; ++h) delete[] labels[h]; delete[] labels;
            delete [] irrep_count;
        }

        fprintf(outfile, "\n  Energy converged.\n");
        fprintf(outfile, "\n  @%s Final Energy: %20.14f",reference.c_str(), E_);
        if (perturb_h_) {
            fprintf(outfile, " with %f perturbation", lambda_);
        }
        fprintf(outfile, "\n");

        // Properties
        if (print_) {
            shared_ptr<OEProp> oe(new OEProp());
            if (print_ < 2)
                oe->add("DIPOLE");
            else
                oe->add("QUADRUPOLE");
        
            if (print_ >= 2)
                oe->add("MULLIKEN_CHARGES");

            fprintf(outfile, "\n  ==> Properties <==\n");
            oe->compute(); 
        }

        save_information();
    } else {
        fprintf(outfile, "\n  Failed to converged.\n");
        E_ = 0.0;
        if(psio_->open_check(PSIF_CHKPT))
            psio_->close(PSIF_CHKPT, 1);
    }

    // Orbitals are always saved, in case a dual basis is required later
    save_orbitals();
    if (options_.get_str("SAPT") != "FALSE") //not a bool because it has types
        save_sapt_info();

    // Clean memory off, handle diis closeout, etc
    finalize();

    //fprintf(outfile,"\nComputation Completed\n");
    fflush(outfile);
    return E_;
}
void HF::diagonalizeFock(shared_ptr<Matrix> Fm, shared_ptr<Matrix> Cm, shared_ptr<Vector> epsm)
{
    for (int h = 0; h < Cm->nirrep(); h++) {

        int nso = nsopi_[h];
        int nmo = nmopi_[h];

        if (nmo == 0 || nso == 0) continue;

        double **Temp = block_matrix(nmo,nso);
        double **Fp = block_matrix(nmo,nmo);
        double **Cp = block_matrix(nmo,nmo);

        double** X = X_->pointer(h);
        double** F = Fm->pointer(h);
        double** C = Cm->pointer(h);

        double* eigvals = epsm->pointer(h);

        //Form F' = X'FX for canonical orthogonalization
        C_DGEMM('T','N',nmo,nso,nso,1.0,X[0],nmo,F[0],nso,0.0,Temp[0],nso);
        C_DGEMM('N','N',nmo,nmo,nso,1.0,Temp[0],nso,X[0],nmo,0.0,Fp[0],nmo);

        //Form C' = eig(F')
        sq_rsp(nmo, nmo, Fp,  eigvals, 1, Cp, 1.0e-14);

        //Form C = XC'
        C_DGEMM('N','N',nso,nmo,nmo,1.0,X[0],nmo,Cp[0],nmo,0.0,C[0],nmo);

        free_block(Temp);
        free_block(Cp);
        free_block(Fp);
    }
}
void HF::reset_SAD_occupation()
{
    // RHF style for now
    for (int h = 0; h < Da_->nirrep(); h++) {
        nalphapi_[h] = sad_nocc_[h];
        nbetapi_[h]  = sad_nocc_[h];
        doccpi_[h]   = sad_nocc_[h];
        soccpi_[h]   = 0;
    }
}
shared_ptr<Matrix> HF::form_Fia(shared_ptr<Matrix> Fso, shared_ptr<Matrix> Cso, int* noccpi)
{
    int* nsopi = Cso->rowspi();
    int* nmopi = Cso->colspi();  
    int nvirpi[nirrep_];

    for (int h = 0; h < nirrep_; h++)
        nvirpi[h] = nmopi[h] - noccpi[h];

    shared_ptr<Matrix> Fia(new Matrix("Fia (Some Basis)", nirrep_, noccpi, nvirpi));

    // Hack to get orbital e for this Fock
    shared_ptr<Matrix> C2(new Matrix("C2", nirrep_, nsopi, nmopi));
    shared_ptr<Vector> E2(new Vector("E2", nirrep_, nmopi));
    diagonalizeFock(Fso, C2, E2);

    for (int h = 0; h < nirrep_; h++) {
        int nmo = nmopi[h];
        int nso = nsopi[h];
        int nvir = nvirpi[h];
        int nocc = noccpi[h]; 
        
        if (nmo == 0 || nso == 0 || nvir == 0 || nocc == 0) continue;

        //double** C = Cso->pointer(h);
        double** C = C2->pointer(h);
        double** F = Fso->pointer(h);
        double** Fiap = Fia->pointer(h);
    
        double** Temp = block_matrix(nocc, nso);

        C_DGEMM('T','N',nocc,nso,nso,1.0,C[0],nmo,F[0],nso,0.0,Temp[0],nso);
        C_DGEMM('N','N',nocc,nvir,nso,1.0,Temp[0],nso,&C[0][nocc],nmo,0.0,Fiap[0],nvir);

        free_block(Temp);

        //double* eps = E2->pointer(h);
        //for (int i = 0; i < nocc; i++)
        //    for (int a = 0; a < nvir; a++)
        //        Fiap[i][a] /= eps[a + nocc] - eps[i];

    }

    //Fia->print();

    return Fia; 
}
shared_ptr<Matrix> HF::form_FDSmSDF(shared_ptr<Matrix> Fso, shared_ptr<Matrix> Dso)
{
    shared_ptr<Matrix> FDSmSDF(new Matrix("FDS-SDF", nirrep_, nsopi_, nsopi_));
    shared_ptr<Matrix> DS(new Matrix("DS", nirrep_, nsopi_, nsopi_));

    DS->gemm(false,false,1.0,Dso,S_,0.0);
    FDSmSDF->gemm(false,false,1.0,Fso,DS,0.0);

    shared_ptr<Matrix> SDF(FDSmSDF->transpose()); 
    FDSmSDF->subtract(SDF);

    DS.reset();
    SDF.reset(); 

    shared_ptr<Matrix> XP(new Matrix("X'(FDS - SDF)", nirrep_, nmopi_, nsopi_));
    shared_ptr<Matrix> XPX(new Matrix("X'(FDS - SDF)X", nirrep_, nmopi_, nmopi_));
    XP->gemm(true,false,1.0,X_,FDSmSDF,0.0);
    XPX->gemm(false,false,1.0,XP,X_,0.0);

    //XPX->print();

    return XPX;
}

}}