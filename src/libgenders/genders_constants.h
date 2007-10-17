/*****************************************************************************\
 *  $Id: genders_constants.h,v 1.3 2007-10-17 17:30:49 chu11 Exp $
 *****************************************************************************
 *  Copyright (C) 2007 Lawrence Livermore National Security, LLC.
 *  Copyright (C) 2001-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jim Garlick <garlick@llnl.gov> and Albert Chu <chu11@llnl.gov>.
 *  UCRL-CODE-2003-004.
 *
 *  This file is part of Genders, a cluster configuration database.
 *  For details, see <http://www.llnl.gov/linux/genders/>.
 *
 *  Genders is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  Genders is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Genders.  If not, see <http://www.gnu.org/licenses/>.
\*****************************************************************************/

#ifndef _GENDERS_CONSTANTS_H
#define _GENDERS_CONSTANTS_H 1

#define GENDERS_MAXHOSTNAMELEN    64

#define GENDERS_BUFLEN            65536

/* Impossible to have a genders value with spaces */
#define GENDERS_NOVALUE           "  NOVAL  "   

#endif /* _GENDERS_CONSTANTS_H */