/* Peter Mell Modified this file   8/95 */

#ifndef lint
static char vcid[] = "$Id: ex9.c,v 1.1 1995/09/02 03:34:17 bsmith Exp bsmith $";
#endif

static char help[] =
"This program demonstrates use of the SNES package to solve systems of\n\
nonlinear equations in parallel.  This example uses matrix free Krylov\n\
Newton methods with no preconditioner.\n\
The Bratu (SFI - solid fuel ignition) test problem\n\
is solved.  The command line options are:\n\
   -par <parameter>, where <parameter> indicates the problem's nonlinearity\n\
      problem SFI:  <parameter> = Bratu parameter (0 <= par <= 6.81)\n\
   -mx <xg>, where <xg> = number of grid points in the x-direction\n\
   -my <yg>, where <yg> = number of grid points in the y-direction\n\
   -mz <zg>, where <zg> = number of grid points in the y-direction\n\n";

/*  
    1) Solid Fuel Ignition (SFI) problem.  This problem is modeled by
    the partial differential equation
  
            -Laplacian u - lambda*exp(u) = 0,  0 < x,y,z < 1 ,
  
    with boundary conditions
   
             u = 0  for  x = 0, x = 1, y = 0, y = 1, z = 0, z = 1.
   
    A finite difference approximation with the usual 7-point stencil
    is used to discretize the boundary value problem to obtain a nonlinear 
    system of equations.
*/

#include "draw.h"
#include "snes.h"
#include "da.h"
#include <math.h>
#include <stdio.h>

typedef struct {
      double        param;         /* test problem parameter */
      int           mx,my,mz;      /* Discretization in x,y-direction */
      Vec           localX,localF; /* ghosted local vector */
      DA            da;            /* regular array datastructure */
} AppCtx;

int  FormFunction1(SNES,Vec,Vec,void*),FormInitialGuess1(SNES,Vec,void*);

int main( int argc, char **argv )
{
  SLES          sles;
  PC            pc;
  SNES          snes;
  SNESMethod    method = SNES_NLS;  /* nonlinear solution method */
  Vec           x,r;
  int           ierr, its, N,Nx = PETSC_DECIDE, Ny = PETSC_DECIDE;
  int           Nz = PETSC_DECIDE; 
  AppCtx        user;
  double        bratu_lambda_max = 6.81, bratu_lambda_min = 0.;
  Mat           J;
  DAStencilType stencil = DA_STENCIL_BOX;

  PetscInitialize( &argc, &argv, 0,0 );
  if (OptionsHasName(0,"-help")) fprintf(stdout,"%s",help);
  if (OptionsHasName(0,"-star")) stencil = DA_STENCIL_STAR;

  user.mx    = 4; 
  user.my    = 4; 
  user.mz    = 4; 
  user.param = 6.0;
  OptionsGetInt(0,"-mx",&user.mx); 
  OptionsGetInt(0,"-my",&user.my);
  OptionsGetInt(0,"-mz",&user.mz);
  OptionsGetInt(0,"-Nx",&Nx); 
  OptionsGetInt(0,"-Ny",&Ny);
  OptionsGetInt(0,"-Nz",&Nz);
  OptionsGetDouble(0,"-par",&user.param);
  if (user.param >= bratu_lambda_max || user.param <= bratu_lambda_min) {
    SETERRA(1,"Lambda is out of range");
  }
  N          = user.mx*user.my*user.mz;
  
  /* Set up distributed array */
  ierr = DACreate3d(MPI_COMM_WORLD,DA_NONPERIODIC,stencil,
                    user.mx,user.my,user.mz,Nx,Ny,Nz,
                    1,1,&user.da); 
  CHKERRA(ierr);
  ierr = DAGetDistributedVector(user.da,&x); CHKERRQ(ierr);
  ierr = VecDuplicate(x,&r); CHKERRA(ierr);
  ierr = DAGetLocalVector(user.da,&user.localX); CHKERRQ(ierr);
  ierr = VecDuplicate(user.localX,&user.localF); CHKERRA(ierr);

  /* Create nonlinear solver */
  ierr = SNESCreate(MPI_COMM_WORLD,SNES_NONLINEAR_EQUATIONS,&snes);
  CHKERRA(ierr);
  ierr = SNESSetMethod(snes,method); CHKERRA(ierr);

  /* Set various routines */
  ierr = SNESSetSolution(snes,x,FormInitialGuess1,(void *)&user); 
           CHKERRA(ierr);
  ierr = SNESSetFunction(snes,r,FormFunction1,(void *)&user,0); 
           CHKERRA(ierr);
  ierr = SNESDefaultMatrixFreeMatCreate(snes,x,&J); CHKERRA(ierr);
  ierr = SNESSetJacobian(snes,J,J,0,(void *)&user); CHKERRA(ierr);

  /* Set up nonlinear solver; then execute it */
  ierr = SNESSetFromOptions(snes); CHKERRA(ierr);

  /* Force no preconditioning to be used. */
  ierr = SNESGetSLES(snes,&sles); CHKERRQ(ierr);
  ierr = SLESGetPC(sles,&pc); CHKERRQ(ierr);
  ierr = PCSetMethod(pc,PCNONE); CHKERRQ(ierr);

  ierr = SNESSetUp(snes); CHKERRA(ierr);
  ierr = SNESSolve(snes,&its);  CHKERRA(ierr);

  MPIU_printf(MPI_COMM_WORLD,"Number of Newton iterations = %d\n", its );

  /* Free data structures */
  ierr = VecDestroy(x); CHKERRA(ierr);
  ierr = VecDestroy(r); CHKERRA(ierr);
  ierr = SNESDestroy(snes); CHKERRA(ierr);
  ierr = DADestroy(user.da); CHKERRQ(ierr);
  PetscFinalize();

  return 0;
}/* --------------------  Form initial approximation ----------------- */
int FormInitialGuess1(SNES snes,Vec X,void *ptr)
{
  AppCtx *user = (AppCtx *) ptr;
  int     i,j,k, loc, mx, my, mz, ierr,xs,ys,zs,xm,ym,zm,Xm,Ym,Zm,Xs,Ys,Zs;
  double  one = 1.0, lambda, temp1, temp, hx, hy, hxdhy, hydhx,sc;
  Scalar  *x;
  Vec     localX = user->localX;
  int     base1;

  mx	 = user->mx; my	 = user->my; mz = user->mz; lambda = user->param;
  hx     = one / (double)(mx-1);     hy     = one / (double)(my-1);
  sc     = hx*hy;
  hxdhy  = hx/hy; hydhx  = hy/hx;

  ierr = VecGetArray(localX,&x); CHKERRQ(ierr);
  temp1 = lambda/(lambda + one);
  DAGetCorners(user->da,&xs,&ys,&zs,&xm,&ym,&zm);
  DAGetGhostCorners(user->da,&Xs,&Ys,&Zs,&Xm,&Ym,&Zm);
 
  for (k=zs; k<zs+zm; k++) {
    base1 = (Xm*Ym)*(k-Zs);
    for (j=ys; j<ys+ym; j++) {
      temp = (double)(PETSCMIN(j,my-j-1))*hy;
      for (i=xs; i<xs+xm; i++) {
        loc = base1 + i-Xs + (j-Ys)*Xm; 
        if (i == 0 || j == 0 || k == 0 || i==mx-1 || j==my-1 || k==mz-1) {
          x[loc] = 0.0; 
          continue;
        }
        x[loc] = temp1*sqrt( PETSCMIN( (double)(PETSCMIN(i,mx-i-1))*hx,temp) ); 
      }
    }
  }

  ierr = VecRestoreArray(localX,&x); CHKERRQ(ierr);
  /* stick values into global vector */
  ierr = DALocalToGlobal(user->da,localX,INSERTVALUES,X);
  return 0;
}/* --------------------  Evaluate Function F(x) --------------------- */
int FormFunction1(SNES snes,Vec X,Vec F,void *ptr)
{
  AppCtx *user = (AppCtx *) ptr;
  int     ierr, i, j, k,loc, mx,my,mz,xs,ys,zs,xm,ym,zm,Xs,Ys,Zs,Xm,Ym,Zm;
  double  two = 2.0, one = 1.0, lambda,hx, hy, hz, hxhzdhy, hyhzdhx,hxhydhz;
  Scalar  u, uxx, uyy, sc;
  Scalar  *x,*f,uzz;
  Vec     localX = user->localX, localF = user->localF; 
  int     base1, base2;

  mx	 = user->mx; my	 = user->my; mz = user->mz; lambda = user->param;
  hx     = one / (double)(mx-1);
  hy     = one / (double)(my-1);
  hz     = one / (double)(mz-1);
  sc     = hx*hy*hz*lambda; hxhzdhy  = hx*hz/hy; hyhzdhx  = hy*hz/hx;
  hxhydhz = hx*hy/hz;

  ierr = DAGlobalToLocalBegin(user->da,X,INSERTVALUES,localX);
  ierr = DAGlobalToLocalEnd(user->da,X,INSERTVALUES,localX);
  ierr = VecGetArray(localX,&x); CHKERRQ(ierr);
  ierr = VecGetArray(localF,&f); CHKERRQ(ierr);

  DAGetCorners(user->da,&xs,&ys,&zs,&xm,&ym,&zm);
  DAGetGhostCorners(user->da,&Xs,&Ys,&Zs,&Xm,&Ym,&Zm);

  for (k=zs; k<zs+zm; k++) {
    base1 = (Xm*Ym)*(k-Zs); 
    for (j=ys; j<ys+ym; j++) {
      base2 = base1 + (j-Ys)*Xm; 
      for (i=xs; i<xs+xm; i++) {
        loc = base2 + (i-Xs);
        if (i == 0 || j == 0 || k== 0 || i == mx-1 || j == my-1 || k == mz-1) {
          f[loc] = x[loc]; 
        }
        else {
          u = x[loc];
          uxx = (two*u - x[loc-1] - x[loc+1])*hyhzdhx;
          uyy = (two*u - x[loc-Xm] - x[loc+Xm])*hxhzdhy;
          uzz = (two*u - x[loc-Xm*Ym] - x[loc+Xm*Ym])*hxhydhz;
          f[loc] = uxx + uyy + uzz - sc*exp(u);
        }
      }  
    }
  }  
  ierr = VecRestoreArray(localX,&x); CHKERRQ(ierr);
  ierr = VecRestoreArray(localF,&f); CHKERRQ(ierr);
  /* stick values into global vector */
  ierr = DALocalToGlobal(user->da,localF,INSERTVALUES,F);
  PLogFlops(11*ym*xm*zm);
  return 0; 
}
 




 





















