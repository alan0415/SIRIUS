// Copyright (c) 2013-2018 Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that
// the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file generate_rho_aug.hpp
 *
 *  \brief Generate augmentation charge.
 */

template <device_t pu>
inline void Density::generate_rho_aug(mdarray<double_complex, 2>& rho_aug__)
{
    PROFILE("sirius::Density::generate_rho_aug");

    rho_aug__.zero((pu == CPU) ? memory_t::host : memory_t::device);

    if (ctx_.unit_cell().atom_type(0).augment() && ctx_.unit_cell().atom_type(0).num_atoms() > 0) {
        ctx_.augmentation_op(0).prepare(stream_id(0));
    }

    for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
        auto& atom_type = unit_cell_.atom_type(iat);

#ifdef __GPU
        if (ctx_.processing_unit() == GPU) {
            acc::sync_stream(stream_id(0));
            if (iat + 1 != unit_cell_.num_atom_types() && ctx_.unit_cell().atom_type(iat + 1).augment() &&
                ctx_.unit_cell().atom_type(iat + 1).num_atoms() > 0) {
                ctx_.augmentation_op(iat + 1).prepare(stream_id(0));
            }
        }
#endif

        if (!atom_type.augment() || atom_type.num_atoms() == 0) {
            continue;
        }

        int nbf = atom_type.mt_basis_size();

        /* convert to real matrix */
        auto dm = density_matrix_aux(iat);

        if (ctx_.control().print_checksum_) {
             auto cs = dm.checksum();
             if (ctx_.comm().rank() == 0) {
                utils::print_checksum("density_matrix_aux", cs);
             }
        }

        if (pu == CPU) {
            utils::timer t2("sirius::Density::generate_rho_aug|phase_fac");
            /* treat phase factors as real array with x2 size */
            mdarray<double, 2> phase_factors(atom_type.num_atoms(), ctx_.gvec().count() * 2);

            #pragma omp parallel for schedule(static)
            for (int igloc = 0; igloc < ctx_.gvec().count(); igloc++) {
                int ig = ctx_.gvec().offset() + igloc;
                for (int i = 0; i < atom_type.num_atoms(); i++) {
                    int ia = atom_type.atom_id(i);
                    double_complex z = std::conj(ctx_.gvec_phase_factor(ig, ia));
                    phase_factors(i, 2 * igloc)     = z.real();
                    phase_factors(i, 2 * igloc + 1) = z.imag();
                }
            }
            t2.stop();
            
            /* treat auxiliary array as double with x2 size */
            mdarray<double, 2> dm_pw(nbf * (nbf + 1) / 2, ctx_.gvec().count() * 2);

            for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
                utils::timer t3("sirius::Density::generate_rho_aug|gemm");
                linalg<CPU>::gemm(0, 0, nbf * (nbf + 1) / 2, 2 * ctx_.gvec().count(), atom_type.num_atoms(), 
                                  &dm(0, 0, iv), dm.ld(),
                                  &phase_factors(0, 0), phase_factors.ld(), 
                                  &dm_pw(0, 0), dm_pw.ld());
                t3.stop();

#ifdef __PRINT_OBJECT_CHECKSUM
                {
                    auto cs = dm_pw.checksum();
                    ctx_.comm().allreduce(&cs, 1);
                    DUMP("checksum(dm_pw) : %18.10f", cs);
                }
#endif

                utils::timer t4("sirius::Density::generate_rho_aug|sum");
                #pragma omp parallel for
                for (int igloc = 0; igloc < ctx_.gvec().count(); igloc++) {
                    double_complex zsum(0, 0);
                    /* get contribution from non-diagonal terms */
                    for (int i = 0; i < nbf * (nbf + 1) / 2; i++) {
                        double_complex z1 = double_complex(ctx_.augmentation_op(iat).q_pw(i, 2 * igloc),
                                                           ctx_.augmentation_op(iat).q_pw(i, 2 * igloc + 1));
                        double_complex z2(dm_pw(i, 2 * igloc), dm_pw(i, 2 * igloc + 1));

                        zsum += z1 * z2 * ctx_.augmentation_op(iat).sym_weight(i);
                    }
                    rho_aug__(igloc, iv) += zsum;
                }
                t4.stop();
            }
        }

#ifdef __GPU
        if (pu == device_t::GPU) {
            dm.allocate(memory_t::device).copy_to(memory_t::device);

            /* treat auxiliary array as double with x2 size */
            mdarray<double, 2> dm_pw(nullptr, nbf * (nbf + 1) / 2, ctx_.gvec().count() * 2);
            dm_pw.allocate(memory_t::device);

            mdarray<double, 1> phase_factors(nullptr, atom_type.num_atoms() * ctx_.gvec().count() * 2);
            phase_factors.allocate(memory_t::device);

            for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
                generate_dm_pw_gpu(atom_type.num_atoms(),
                                   ctx_.gvec().count(),
                                   nbf,
                                   ctx_.unit_cell().atom_coord(iat).at(memory_t::device),
                                   ctx_.gvec_coord().at(memory_t::device),
                                   phase_factors.at(memory_t::device),
                                   dm.at(memory_t::device, 0, 0, iv),
                                   dm_pw.at(memory_t::device),
                                   1);
                sum_q_pw_dm_pw_gpu(ctx_.gvec().count(), 
                                   nbf,
                                   ctx_.augmentation_op(iat).q_pw().at(memory_t::device),
                                   dm_pw.at(memory_t::device),
                                   ctx_.augmentation_op(iat).sym_weight().at(memory_t::device),
                                   rho_aug__.at(memory_t::device, 0, iv),
                                   1);
            }
            acc::sync_stream(stream_id(1));
            ctx_.augmentation_op(iat).dismiss();
        }
#endif
    }

    if (pu == device_t::GPU) {
        rho_aug__.copy_to(memory_t::host);
    }

    if (ctx_.control().print_checksum_) {
         auto cs = rho_aug__.checksum();
         ctx_.comm().allreduce(&cs, 1);
         if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_aug", cs);
         }
    }

    if (ctx_.control().print_hash_) {
         auto h = rho_aug__.hash();
         if (ctx_.comm().rank() == 0) {
            utils::print_hash("rho_aug", h);
         }
    }
}

