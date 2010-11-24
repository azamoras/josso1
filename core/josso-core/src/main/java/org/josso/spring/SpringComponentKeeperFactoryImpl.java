/*
 * JOSSO: Java Open Single Sign-On
 *
 * Copyright 2004-2009, Atricore, Inc.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 *
 */

package org.josso.spring;

import org.josso.ComponentKeeper;
import org.josso.ComponentKeeperFactory;

/**
 * Date: Sep 5, 2007
 * Time: 10:00:00 AM
 *
 * @author <a href="mailto:ggarcia@josso.org">Gustavo Garcia</a>
 */
public class SpringComponentKeeperFactoryImpl extends ComponentKeeperFactory {
    public ComponentKeeper newComponentKeeper() {
        return new SpringComponentKeeperImpl(super.getResourceFileName());
    }
}