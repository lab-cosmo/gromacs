/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright 2024- The GROMACS Authors
 * and the project initiators Erik Lindahl, Berk Hess and David van der Spoel.
 * Consult the AUTHORS/COPYING files and https://www.gromacs.org for details.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * https://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at https://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out https://www.gromacs.org.
 */

/*! \internal \file
 * \brief
 * Implements metatensor MDModule class when metatensor is not linked
 *
 * \author Guillaume Fraux <guillaume.fraux@epfl.ch>
 * \ingroup module_applied_forces
 */

#include "gmxpre.h"

#include "gromacs/mdtypes/imdmodule.h"

#include "metatensor_mdmodule.h"


namespace gmx
{

namespace
{

/*! \internal
 * \brief Metatensor module
 *
 * Stub Implementation in case metatensor is not compiled
 */
class MetatensorModule final : public IMDModule
{
public:
    //! \brief Construct the colvars module.
    explicit MetatensorModule() = default;

    void subscribeToPreProcessingNotifications(MDModulesNotifiers* /*notifier*/) override {}
    void subscribeToSimulationSetupNotifications(MDModulesNotifiers* /*notifier*/) override {}

    IMdpOptionProvider* mdpOptionProvider() override { return nullptr; }
    IMDOutputProvider* outputProvider() override { return nullptr; }

    void initForceProviders(ForceProviders* /*forceProviders*/) override {}
private:
};


} // namespace

std::unique_ptr<IMDModule> MetatensorModuleInfo::create()
{
    return std::make_unique<MetatensorModule>();
}

const std::string MetatensorModuleInfo::name_ = "metatensor";

} // namespace gmx
