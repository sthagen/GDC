
/* Compiler implementation of the D programming language
 * Copyright (c) 1999-2014 by Digital Mars
 * All Rights Reserved
 * written by Walter Bright
 * http://www.digitalmars.com
 * Distributed under the Boost Software License, Version 1.0.
 * http://www.boost.org/LICENSE_1_0.txt
 * https://github.com/D-Programming-Language/dmd/blob/master/src/struct.c
 */

#include <stdio.h>
#include <assert.h>

#include "root.h"
#include "aggregate.h"
#include "scope.h"
#include "mtype.h"
#include "init.h"
#include "declaration.h"
#include "module.h"
#include "id.h"
#include "statement.h"
#include "template.h"
#include "tokens.h"

Type *getTypeInfoType(Type *t, Scope *sc);
TypeTuple *toArgTypes(Type *t);

FuncDeclaration *StructDeclaration::xerreq;     // object.xopEquals
FuncDeclaration *StructDeclaration::xerrcmp;    // object.xopCmp

/***************************************
 * Search toString member function for TypeInfo_Struct.
 *      string toString();
 */
FuncDeclaration *search_toString(StructDeclaration *sd)
{
    Dsymbol *s = search_function(sd, Id::tostring);
    FuncDeclaration *fd = s ? s->isFuncDeclaration() : NULL;
    if (fd)
    {
        static TypeFunction *tftostring;
        if (!tftostring)
        {
            tftostring = new TypeFunction(NULL, Type::tstring, 0, LINKd);
            tftostring = (TypeFunction *)tftostring->merge();
        }

        fd = fd->overloadExactMatch(tftostring);
    }
    return fd;
}

/***************************************
 * Request additonal semantic analysis for TypeInfo generation.
 */
void semanticTypeInfo(Scope *sc, Type *t)
{
    class FullTypeInfoVisitor : public Visitor
    {
    public:
        Scope *sc;

        void visit(Type *t)
        {
            Type *tb = t->toBasetype();
            if (tb != t)
                tb->accept(this);
        }
        void visit(TypeNext *t)
        {
            if (t->next)
                t->next->accept(this);
        }
        void visit(TypeBasic *t) { }
        void visit(TypeVector *t)
        {
            t->basetype->accept(this);
        }
        void visit(TypeAArray *t)
        {
            t->index->accept(this);
            visit((TypeNext *)t);
        }
        void visit(TypeFunction *t)
        {
            visit((TypeNext *)t);
            // Currently TypeInfo_Function doesn't store parameter types.
        }
        void visit(TypeStruct *t)
        {
            StructDeclaration *sd = t->sym;
            if (!sd->members)
                return;     // opaque struct
            if (sd->semanticRun >= PASSsemantic3)
                return;     // semantic3 will be done
            if (!sd->xeq && !sd->xcmp && !sd->postblit &&
                !sd->dtor && !sd->xhash && !search_toString(sd))
                return;     // none of TypeInfo-specific members

            // If the struct is in a non-root module, run semantic3 to get
            // correct symbols for the member function.
            if (TemplateInstance *ti = sd->isInstantiated())
            {
                if (ti->minst && !ti->minst->isRoot())
                    Module::addDeferredSemantic3(sd);
            }
            else
            {
                if (sd->inNonRoot())
                {
                    //printf("deferred sem3 for TypeInfo - sd = %s, inNonRoot = %d\n", sd->toChars(), sd->inNonRoot());
                    Module::addDeferredSemantic3(sd);
                }
            }

            if (!sc)    // inline may request TypeInfo.
            {
                Scope scx;
                scx.module = sd->getModule();
                getTypeInfoType(t, &scx);
            }
            else
                getTypeInfoType(t, sc);

            if (!sc || sc->minst)
                sd->requestTypeInfo = true;
        }
        void visit(TypeClass *t) { }
        void visit(TypeTuple *t)
        {
            if (t->arguments)
            {
                for (size_t i = 0; i < t->arguments->dim; i++)
                {
                    Type *tprm = (*t->arguments)[i]->type;
                    if (tprm)
                        tprm->accept(this);
                }
            }
        }
    };
    FullTypeInfoVisitor v;
    v.sc = sc;
    t->accept(&v);
}

/********************************* AggregateDeclaration ****************************/

AggregateDeclaration::AggregateDeclaration(Loc loc, Identifier *id)
    : ScopeDsymbol(id)
{
    this->loc = loc;

    storage_class = 0;
    protection = Prot(PROTpublic);
    type = NULL;
    structsize = 0;             // size of struct
    alignsize = 0;              // size of struct for alignment purposes
    sizeok = SIZEOKnone;        // size not determined yet
    deferred = NULL;
    isdeprecated = false;
    inv = NULL;
    aggNew = NULL;
    aggDelete = NULL;

    stag = NULL;
    sinit = NULL;
    enclosing = NULL;
    vthis = NULL;

    ctor = NULL;
    defaultCtor = NULL;
    aliasthis = NULL;
    noDefaultCtor = false;
    dtor = NULL;
    getRTInfo = NULL;
}

Prot AggregateDeclaration::prot()
{
    return protection;
}

void AggregateDeclaration::setScope(Scope *sc)
{
    if (sizeok == SIZEOKdone)
        return;
    ScopeDsymbol::setScope(sc);
}

/***************************************
 * Create a new scope from sc.
 * semantic, semantic2 and semantic3 will use this for aggregate members.
 */
Scope *AggregateDeclaration::newScope(Scope *sc)
{
    Scope *sc2 = sc->push(this);
    sc2->stc &= STCsafe | STCtrusted | STCsystem;
    sc2->parent = this;
    if (isUnionDeclaration())
        sc2->inunion = 1;
    sc2->protection = Prot(PROTpublic);
    sc2->explicitProtection = 0;
    sc2->aligndecl = NULL;
    sc2->userAttribDecl = NULL;
    return sc2;
}

void AggregateDeclaration::semantic2(Scope *sc)
{
    //printf("AggregateDeclaration::semantic2(%s) type = %s, errors = %d\n", toChars(), type->toChars(), errors);
    if (!members)
        return;

    if (_scope && sizeok == SIZEOKfwd)   // Bugzilla 12531
        semantic(NULL);
    if (_scope)
    {
        error("has forward references");
        return;
    }

    Scope *sc2 = newScope(sc);

    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        //printf("\t[%d] %s\n", i, s->toChars());
        s->semantic2(sc2);
    }

    sc2->pop();
}

void AggregateDeclaration::semantic3(Scope *sc)
{
    //printf("AggregateDeclaration::semantic3(%s) type = %s, errors = %d\n", toChars(), type->toChars(), errors);
    if (!members)
        return;

    StructDeclaration *sd = isStructDeclaration();
    if (!sc)    // from runDeferredSemantic3 for TypeInfo generation
    {
        assert(sd);
        sd->semanticTypeInfoMembers();
        return;
    }

    Scope *sc2 = newScope(sc);

    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        s->semantic3(sc2);
    }

    sc2->pop();

    // don't do it for unused deprecated types
    // or error types
    if (!getRTInfo && Type::rtinfo &&
        (!isDeprecated() || global.params.useDeprecated) &&
        (type && type->ty != Terror))
    {
        // Evaluate: RTinfo!type
        Objects *tiargs = new Objects();
        tiargs->push(type);
        TemplateInstance *ti = new TemplateInstance(loc, Type::rtinfo, tiargs);

        Scope *sc3 = ti->tempdecl->_scope->startCTFE();
        sc3->tinst = sc->tinst;
        sc3->minst = sc->minst;
        if (isDeprecated())
            sc3->stc |= STCdeprecated;

        ti->semantic(sc);
        ti->semantic2(sc);
        ti->semantic3(sc);
        Expression *e = DsymbolExp::resolve(Loc(), sc3, ti->toAlias(), false);

        sc3->endCTFE();

        e = e->ctfeInterpret();
        getRTInfo = e;
    }

    if (sd)
        sd->semanticTypeInfoMembers();
}

void StructDeclaration::semanticTypeInfoMembers()
{
    if (xeq &&
        xeq->_scope &&
        xeq->semanticRun < PASSsemantic3done)
    {
        unsigned errors = global.startGagging();
        xeq->semantic3(xeq->_scope);
        if (global.endGagging(errors))
            xeq = xerreq;
    }

    if (xcmp &&
        xcmp->_scope &&
        xcmp->semanticRun < PASSsemantic3done)
    {
        unsigned errors = global.startGagging();
        xcmp->semantic3(xcmp->_scope);
        if (global.endGagging(errors))
            xcmp = xerrcmp;
    }

    FuncDeclaration *ftostr = search_toString(this);
    if (ftostr &&
        ftostr->_scope &&
        ftostr->semanticRun < PASSsemantic3done)
    {
        ftostr->semantic3(ftostr->_scope);
    }

    if (xhash &&
        xhash->_scope &&
        xhash->semanticRun < PASSsemantic3done)
    {
        xhash->semantic3(xhash->_scope);
    }

    if (postblit &&
        postblit->_scope &&
        postblit->semanticRun < PASSsemantic3done)
    {
        postblit->semantic3(postblit->_scope);
    }

    if (dtor &&
        dtor->_scope &&
        dtor->semanticRun < PASSsemantic3done)
    {
        dtor->semantic3(dtor->_scope);
    }
}

unsigned AggregateDeclaration::size(Loc loc)
{
    //printf("AggregateDeclaration::size() %s, scope = %p\n", toChars(), _scope);
    if (loc.linnum == 0)
        loc = this->loc;
    if (sizeok != SIZEOKdone && _scope)
    {
        semantic(NULL);

        // Determine the instance size of base class first.
        if (ClassDeclaration *cd = isClassDeclaration())
            cd->baseClass->size(loc);
    }

    if (sizeok != SIZEOKdone && members)
    {
        /* See if enough is done to determine the size,
         * meaning all the fields are done.
         */
        struct SV
        {
            /* Returns:
             *  0       this member doesn't need further processing to determine struct size
             *  1       this member does
             */
            static int func(Dsymbol *s, void *param)
            {
                VarDeclaration *v = s->isVarDeclaration();
                if (v)
                {
                    /* Bugzilla 12799: enum a = ...; is a VarDeclaration and
                     * STCmanifest is already set in parssing stage. So we can
                     * check this before the semantic() call.
                     */
                    if (v->storage_class & STCmanifest)
                        return 0;

                    if (v->_scope)
                        v->semantic(NULL);
                    if (v->storage_class & (STCstatic | STCextern | STCtls | STCgshared | STCmanifest | STCctfe | STCtemplateparameter))
                        return 0;
                    if (v->isField() && v->semanticRun >= PASSsemanticdone)
                        return 0;
                    return 1;
                }
                return 0;
            }
        };
        SV sv;

        for (size_t i = 0; i < members->dim; i++)
        {
            Dsymbol *s = (*members)[i];
            if (s->apply(&SV::func, &sv))
                goto L1;
        }
        finalizeSize();

      L1: ;
    }

    if (!members)
    {
        error(loc, "unknown size");
    }
    else if (sizeok != SIZEOKdone)
    {
        error(loc, "no size yet for forward reference");
        //*(char*)0=0;
    }
    return structsize;
}

Type *AggregateDeclaration::getType()
{
    return type;
}

bool AggregateDeclaration::isDeprecated()
{
    return isdeprecated;
}

bool AggregateDeclaration::isExport()
{
    return protection.kind == PROTexport;
}

/***************************************
 * Calculate field[i].overlapped, and check that all of explicit
 * field initializers have unique memory space on instance.
 * Returns:
 *      true if any errors happen.
 */

bool AggregateDeclaration::checkOverlappedFields()
{
    //printf("AggregateDeclaration::checkOverlappedFields() %s\n", toChars());
    assert(sizeok == SIZEOKdone);
    size_t nfields = fields.dim;
    if (isNested())
    {
        ClassDeclaration *cd = isClassDeclaration();
        if (!cd || !cd->baseClass || !cd->baseClass->isNested())
            nfields--;
    }
    bool errors = false;

    // Fill in missing any elements with default initializers
    for (size_t i = 0; i < nfields; i++)
    {
        VarDeclaration *vd = fields[i];
        VarDeclaration *vx = vd;
        if (vd->_init && vd->_init->isVoidInitializer())
            vx = NULL;

        // Find overlapped fields with the hole [vd->offset .. vd->offset->size()].
        for (size_t j = 0; j < nfields; j++)
        {
            if (i == j)
                continue;
            VarDeclaration *v2 = fields[j];
            if (!vd->isOverlappedWith(v2))
                continue;

            // vd and v2 are overlapping. If either has destructors, postblits, etc., then error
            //printf("overlapping fields %s and %s\n", vd->toChars(), v2->toChars());
            for (size_t k = 0; k < 2; k++)
            {
                VarDeclaration *v = k == 0 ? vd : v2;
                Type *tv = v->type->baseElemOf();
                Dsymbol *sv = tv->toDsymbol(NULL);
                if (!sv || errors)
                    continue;
                StructDeclaration *sd = sv->isStructDeclaration();
                if (sd && (sd->dtor || sd->inv || sd->postblit))
                {
                    error("destructors, postblits and invariants are not allowed in overlapping fields %s and %s",
                          vd->toChars(), v2->toChars());
                    errors = true;
                    break;
                }
            }
            vd->overlapped = true;

            if (!vx)
                continue;
            if (v2->_init && v2->_init->isVoidInitializer())
                continue;

            if (vx->_init && v2->_init)
            {
                ::error(loc, "overlapping default initialization for field %s and %s", v2->toChars(), vd->toChars());
                errors = true;
            }
        }
    }
    return errors;
}

/***************************************
 * Fill out remainder of elements[] with default initializers for fields[].
 * Input:
 *      loc:        location
 *      elements:   explicit arguments which given to construct object.
 *      ctorinit:   true if the elements will be used for default initialization.
 * Returns:
 *      false if any errors occur.
 *      Otherwise, returns true and the missing arguments will be pushed in elements[].
 */
bool AggregateDeclaration::fill(Loc loc, Expressions *elements, bool ctorinit)
{
    //printf("AggregateDeclaration::fill() %s\n", toChars());
    assert(sizeok == SIZEOKdone);
    assert(elements);
    size_t nfields = fields.dim - isNested();
    bool errors = false;

    size_t dim = elements->dim;
    elements->setDim(nfields);
    for (size_t i = dim; i < nfields; i++)
        (*elements)[i] = NULL;

    // Fill in missing any elements with default initializers
    for (size_t i = 0; i < nfields; i++)
    {
        if ((*elements)[i])
            continue;

        VarDeclaration *vd = fields[i];
        VarDeclaration *vx = vd;
        if (vd->_init && vd->_init->isVoidInitializer())
            vx = NULL;

        // Find overlapped fields with the hole [vd->offset .. vd->offset->size()].
        size_t fieldi = i;
        for (size_t j = 0; j < nfields; j++)
        {
            if (i == j)
                continue;
            VarDeclaration *v2 = fields[j];
            if (!vd->isOverlappedWith(v2))
                continue;

            if ((*elements)[j])
            {
                vx = NULL;
                break;
            }
            if (v2->_init && v2->_init->isVoidInitializer())
                continue;

            if (1)
            {
                /* Prefer first found non-void-initialized field
                 * union U { int a; int b = 2; }
                 * U u;    // Error: overlapping initialization for field a and b
                 */
                if (!vx)
                {
                    vx = v2;
                    fieldi = j;
                }
                else if (v2->_init)
                {
                    ::error(loc, "overlapping initialization for field %s and %s",
                        v2->toChars(), vd->toChars());
                    errors = true;
                }
            }
            else
            {
                // Will fix Bugzilla 1432 by enabling this path always

                /* Prefer explicitly initialized field
                 * union U { int a; int b = 2; }
                 * U u;    // OK (u.b == 2)
                 */
                if (!vx || !vx->_init && v2->_init)
                {
                    vx = v2;
                    fieldi = j;
                }
                else if (vx != vd && !vx->isOverlappedWith(v2))
                {
                    // Both vx and v2 fills vd, but vx and v2 does not overlap
                }
                else if (vx->_init && v2->_init)
                {
                    ::error(loc, "overlapping default initialization for field %s and %s",
                        v2->toChars(), vd->toChars());
                }
                else
                    assert(vx->_init || !vx->_init && !v2->_init);
            }
        }
        if (vx)
        {
            Expression *e;
            if (vx->type->size() == 0)
            {
                e = NULL;
            }
            else if (vx->_init)
            {
                assert(!vx->_init->isVoidInitializer());
                e = vx->getConstInitializer(false);
            }
            else
            {
                if ((vx->storage_class & STCnodefaultctor) && !ctorinit)
                {
                    ::error(loc, "field %s.%s must be initialized because it has no default constructor",
                            type->toChars(), vx->toChars());
                    errors = true;
                }

                /* Bugzilla 12509: Get the element of static array type.
                 */
                Type *telem = vx->type;
                if (telem->ty == Tsarray)
                {
                    /* We cannot use Type::baseElemOf() here.
                     * If the bottom of the Tsarray is an enum type, baseElemOf()
                     * will return the base of the enum, and its default initializer
                     * would be different from the enum's.
                     */
                    while (telem->toBasetype()->ty == Tsarray)
                        telem = ((TypeSArray *)telem->toBasetype())->next;

                    if (telem->ty == Tvoid)
                        telem = Type::tuns8->addMod(telem->mod);
                }
                if (telem->needsNested() && ctorinit)
                    e = telem->defaultInit(loc);
                else
                    e = telem->defaultInitLiteral(loc);
            }
            (*elements)[fieldi] = e;
        }
    }

    for (size_t i = 0; i < elements->dim; i++)
    {
        Expression *e = (*elements)[i];
        if (e && e->op == TOKerror)
            return false;
    }

    return !errors;
}

/****************************
 * Do byte or word alignment as necessary.
 * Align sizes of 0, as we may not know array sizes yet.
 *
 * alignment: struct alignment that is in effect
 * size: alignment requirement of field
 */

void AggregateDeclaration::alignmember(
        structalign_t alignment,
        unsigned size,
        unsigned *poffset)
{
    //printf("alignment = %d, size = %d, offset = %d\n",alignment,size,offset);
    switch (alignment)
    {
        case (structalign_t) 1:
            // No alignment
            break;

        case (structalign_t) STRUCTALIGN_DEFAULT:
            // Alignment in Target::fieldalignsize must match what the
            // corresponding C compiler's default alignment behavior is.
            assert(size > 0 && !(size & (size - 1)));
            *poffset = (*poffset + size - 1) & ~(size - 1);
            break;

        default:
            // Align on alignment boundary, which must be a positive power of 2
            assert(alignment > 0 && !(alignment & (alignment - 1)));
            *poffset = (*poffset + alignment - 1) & ~(alignment - 1);
            break;
    }
}

/****************************************
 * Place a member (mem) into an aggregate (agg), which can be a struct, union or class
 * Returns:
 *      offset to place field at
 *
 * nextoffset:    next location in aggregate
 * memsize:       size of member
 * memalignsize:  size of member for alignment purposes
 * alignment:     alignment in effect for this member
 * paggsize:      size of aggregate (updated)
 * paggalignsize: size of aggregate for alignment purposes (updated)
 * isunion:       the aggregate is a union
 */
unsigned AggregateDeclaration::placeField(
        unsigned *nextoffset,
        unsigned memsize,
        unsigned memalignsize,
        structalign_t alignment,
        unsigned *paggsize,
        unsigned *paggalignsize,
        bool isunion
        )
{
    unsigned ofs = *nextoffset;
    alignmember(alignment, memalignsize, &ofs);
    unsigned memoffset = ofs;
    ofs += memsize;
    if (ofs > *paggsize)
        *paggsize = ofs;
    if (!isunion)
        *nextoffset = ofs;

    if (alignment == STRUCTALIGN_DEFAULT)
    {
        if (global.params.is64bit && memalignsize == 16)
            ;
        else if (8 < memalignsize)
            memalignsize = 8;
    }
    else
    {
        if (memalignsize < alignment)
            memalignsize = alignment;
    }

    if (*paggalignsize < memalignsize)
        *paggalignsize = memalignsize;

    return memoffset;
}


/****************************************
 * Returns true if there's an extra member which is the 'this'
 * pointer to the enclosing context (enclosing aggregate or function)
 */

bool AggregateDeclaration::isNested()
{
    return enclosing != NULL;
}

void AggregateDeclaration::makeNested()
{
    if (enclosing)  // if already nested
        return;
    if (sizeok == SIZEOKdone)
        return;
    if (isUnionDeclaration() || isInterfaceDeclaration())
        return;
    if (storage_class & STCstatic)
        return;

    // If nested struct, add in hidden 'this' pointer to outer scope
    Dsymbol *s = toParent2();
    if (!s)
        return;
    AggregateDeclaration *ad = s->isAggregateDeclaration();
    FuncDeclaration *fd = s->isFuncDeclaration();
    Type *t = NULL;
    if (fd)
    {
        enclosing = fd;

        AggregateDeclaration *agg = fd->isMember2();
        t = agg ? agg->handleType() : Type::tvoidptr;
    }
    else if (ad)
    {
        if (isClassDeclaration() && ad->isClassDeclaration())
        {
            enclosing = ad;
        }
        else if (isStructDeclaration())
        {
            if (TemplateInstance *ti = ad->parent->isTemplateInstance())
            {
                enclosing = ti->enclosing;
            }
        }

        t = ad->handleType();
    }
    if (enclosing)
    {
        //printf("makeNested %s, enclosing = %s\n", toChars(), enclosing->toChars());
        assert(t);
        if (t->ty == Tstruct)
            t = Type::tvoidptr;     // t should not be a ref type
        assert(!vthis);
        vthis = new ThisDeclaration(loc, t);
        //vthis->storage_class |= STCref;
        members->push(vthis);
    }
}

/*******************************************
 * Look for constructor declaration.
 */
Dsymbol *AggregateDeclaration::searchCtor()
{
    Dsymbol *s = search(Loc(), Id::ctor);
    if (s)
    {
        if (!(s->isCtorDeclaration() ||
              s->isTemplateDeclaration() ||
              s->isOverloadSet()))
        {
            s->error("is not a constructor; identifiers starting with __ are reserved for the implementation");
            errors = true;
            s = NULL;
        }
    }
    return s;
}

/********************************* StructDeclaration ****************************/

StructDeclaration::StructDeclaration(Loc loc, Identifier *id, bool inObject)
    : AggregateDeclaration(loc, id)
{
    zeroInit = 0;       // assume false until we do semantic processing
    hasIdentityAssign = false;
    hasIdentityEquals = false;
    postblit = NULL;

    xeq = NULL;
    xcmp = NULL;
    xhash = NULL;
    alignment = 0;
    ispod = ISPODfwd;
    arg1type = NULL;
    arg2type = NULL;
    requestTypeInfo = false;

    // For forward references
    type = new TypeStruct(this);

    if (inObject)
    {
        if (id == Id::ModuleInfo && !Module::moduleinfo)
            Module::moduleinfo = this;
#ifdef IN_GCC
        else if (id == Id::Interface && !Type::typeinterface)
            Type::typeinterface = this;
#endif
    }
}

Dsymbol *StructDeclaration::syntaxCopy(Dsymbol *s)
{
    StructDeclaration *sd =
        s ? (StructDeclaration *)s
          : new StructDeclaration(loc, ident);
    return ScopeDsymbol::syntaxCopy(sd);
}

void StructDeclaration::semantic(Scope *sc)
{
    //printf("+StructDeclaration::semantic(this=%p, %s '%s', sizeok = %d)\n", this, parent->toChars(), toChars(), sizeok);

    //static int count; if (++count == 20) halt();

    if (semanticRun >= PASSsemanticdone)
        return;
    unsigned dprogress_save = Module::dprogress;
    int errors = global.errors;

    Scope *scx = NULL;
    if (_scope)
    {
        sc = _scope;
        scx = _scope;            // save so we don't make redundant copies
        _scope = NULL;
    }

    if (!parent)
    {
        assert(sc->parent && sc->func);
        parent = sc->parent;
    }
    assert(parent && !isAnonymous());
    type = type->semantic(loc, sc);

    if (type->ty == Tstruct && ((TypeStruct *)type)->sym != this)
    {
        TemplateInstance *ti = ((TypeStruct *)type)->sym->isInstantiated();
        if (ti && isError(ti))
            ((TypeStruct *)type)->sym = this;
    }

    // Ungag errors when not speculative
    Ungag ungag = ungagSpeculative();

    if (semanticRun == PASSinit)
    {
        protection = sc->protection;

        alignment = sc->alignment();

        storage_class |= sc->stc;
        if (storage_class & STCdeprecated)
            isdeprecated = true;
        if (storage_class & STCabstract)
            error("structs, unions cannot be abstract");
        userAttribDecl = sc->userAttribDecl;
    }
    else if (symtab && !scx)
    {
        semanticRun = PASSsemanticdone;
        return;
    }
    semanticRun = PASSsemantic;

    if (!members)               // if opaque declaration
    {
        semanticRun = PASSsemanticdone;
        return;
    }
    if (!symtab)
        symtab = new DsymbolTable();

    if (sizeok == SIZEOKnone)            // if not already done the addMember step
    {
        for (size_t i = 0; i < members->dim; i++)
        {
            Dsymbol *s = (*members)[i];
            //printf("adding member '%s' to '%s'\n", s->toChars(), this->toChars());
            s->addMember(sc, this);
        }
    }

    Scope *sc2 = newScope(sc);

    if (sizeok == SIZEOKdone)
        goto LafterSizeok;

    sizeok = SIZEOKnone;

    /* Set scope so if there are forward references, we still might be able to
     * resolve individual members like enums.
     */
    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        //printf("struct: setScope %s %s\n", s->kind(), s->toChars());
        s->setScope(sc2);
    }

    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        s->importAll(sc2);
    }

    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        s->semantic(sc2);
    }
    finalizeSize();

    if (sizeok == SIZEOKfwd)
    {
        // semantic() failed because of forward references.
        // Unwind what we did, and defer it for later
        for (size_t i = 0; i < fields.dim; i++)
        {
            VarDeclaration *v = fields[i];
            v->offset = 0;
        }
        fields.setDim(0);
        structsize = 0;
        alignsize = 0;

        sc2->pop();

        _scope = scx ? scx : sc->copy();
        _scope->setNoFree();
        _scope->module->addDeferredSemantic(this);

        Module::dprogress = dprogress_save;
        //printf("\tdeferring %s\n", toChars());
        return;
    }

    Module::dprogress++;

    //printf("-StructDeclaration::semantic(this=%p, '%s')\n", this, toChars());

LafterSizeok:
    // The additions of special member functions should have its own
    // sub-semantic analysis pass, and have to be deferred sometimes.
    // See the case in compilable/test14838.d
    for (size_t i = 0; i < fields.dim; i++)
    {
        VarDeclaration *v = fields[i];
        Type *tb = v->type->baseElemOf();
        if (tb->ty != Tstruct)
            continue;
        StructDeclaration *sd = ((TypeStruct *)tb)->sym;
        if (sd->semanticRun >= PASSsemanticdone)
            continue;

        sc2->pop();

        _scope = scx ? scx : sc->copy();
        _scope->setNoFree();
        _scope->module->addDeferredSemantic(this);

        //printf("\tdeferring %s\n", toChars());
        return;
    }

    /* Look for special member functions.
     */
    aggNew =       (NewDeclaration *)search(Loc(), Id::classNew);
    aggDelete = (DeleteDeclaration *)search(Loc(), Id::classDelete);

    // this->ctor is already set in finalizeSize()

    dtor = buildDtor(this, sc2);
    postblit = buildPostBlit(this, sc2);

    buildOpAssign(this, sc2);
    buildOpEquals(this, sc2);

    xeq = buildXopEquals(this, sc2);
    xcmp = buildXopCmp(this, sc2);
    xhash = buildXtoHash(this, sc2);

    /* Even if the struct is merely imported and its semantic3 is not run,
     * the TypeInfo object would be speculatively stored in each object
     * files. To set correct function pointer, run semantic3 for xeq and xcmp.
     */
    //if ((xeq && xeq != xerreq || xcmp && xcmp != xerrcmp) && isImportedSym(this))
    //    Module::addDeferredSemantic3(this);
    /* Defer requesting semantic3 until TypeInfo generation is actually invoked.
     * See semanticTypeInfo().
     */
    inv = buildInv(this, sc2);

    sc2->pop();

    if (ctor)
    {
        Dsymbol *scall = search(Loc(), Id::call);
        if (scall)
        {
            unsigned xerrors = global.startGagging();
            sc = sc->push();
            sc->tinst = NULL;
            sc->minst = NULL;
            FuncDeclaration *fcall = resolveFuncCall(loc, sc, scall, NULL, NULL, NULL, 1);
            sc = sc->pop();
            global.endGagging(xerrors);

            if (fcall && fcall->isStatic())
            {
                error(fcall->loc, "static opCall is hidden by constructors and can never be called");
                errorSupplemental(fcall->loc, "Please use a factory method instead, or replace all constructors with static opCall.");
            }
        }
    }

    Module::dprogress++;
    semanticRun = PASSsemanticdone;

    TypeTuple *tup = toArgTypes(type);
    size_t dim = tup->arguments->dim;
    if (dim >= 1)
    {
        assert(dim <= 2);
        arg1type = (*tup->arguments)[0]->type;
        if (dim == 2)
            arg2type = (*tup->arguments)[1]->type;
    }

    if (sc->func)
        semantic2(sc);

    if (global.errors != errors)
    {
        // The type is no good.
        type = Type::terror;
        this->errors = true;
        if (deferred)
            deferred->errors = true;
    }

    if (deferred && !global.gag)
    {
        deferred->semantic2(sc);
        deferred->semantic3(sc);
    }

#if 0
    if (type->ty == Tstruct && ((TypeStruct *)type)->sym != this)
    {
        printf("this = %p %s\n", this, this->toChars());
        printf("type = %d sym = %p\n", type->ty, ((TypeStruct *)type)->sym);
    }
#endif
    assert(type->ty != Tstruct || ((TypeStruct *)type)->sym == this);
}

Dsymbol *StructDeclaration::search(Loc loc, Identifier *ident, int flags)
{
    //printf("%s.StructDeclaration::search('%s')\n", toChars(), ident->toChars());

    if (_scope && !symtab)
        semantic(_scope);

    if (!members || !symtab)    // opaque or semantic() is not yet called
    {
        error("is forward referenced when looking for '%s'", ident->toChars());
        return NULL;
    }

    return ScopeDsymbol::search(loc, ident, flags);
}

void StructDeclaration::finalizeSize()
{
    //printf("StructDeclaration::finalizeSize() %s\n", toChars());
    if (sizeok != SIZEOKnone)
        return;

    // Set the offsets of the fields and determine the size of the struct
    unsigned offset = 0;
    bool isunion = isUnionDeclaration() != NULL;
    for (size_t i = 0; i < members->dim; i++)
    {
        Dsymbol *s = (*members)[i];
        s->setFieldOffset(this, &offset, isunion);
    }
    if (sizeok == SIZEOKfwd)
        return;

    // 0 sized struct's are set to 1 byte
    if (structsize == 0)
    {
        structsize = 1;
        alignsize = 1;
    }

    // Round struct size up to next alignsize boundary.
    // This will ensure that arrays of structs will get their internals
    // aligned properly.
    if (alignment == STRUCTALIGN_DEFAULT)
        structsize = (structsize + alignsize - 1) & ~(alignsize - 1);
    else
        structsize = (structsize + alignment - 1) & ~(alignment - 1);

    sizeok = SIZEOKdone;

    // Calculate fields[i]->overlapped
    checkOverlappedFields();

    // Determine if struct is all zeros or not
    zeroInit = 1;
    for (size_t i = 0; i < fields.dim; i++)
    {
        VarDeclaration *vd = fields[i];
        if (!vd->isDataseg())
        {
            if (vd->_init)
            {
                // Should examine init to see if it is really all 0's
                zeroInit = 0;
                break;
            }
            else
            {
                if (!vd->type->isZeroInit(loc))
                {
                    zeroInit = 0;
                    break;
                }
            }
        }
    }

    // Look for the constructor, for the struct literal/constructor call expression
    ctor = searchCtor();
    if (ctor)
    {
        // Finish all constructors semantics to determine this->noDefaultCtor.
        struct SearchCtor
        {
            static int fp(Dsymbol *s, void *ctxt)
            {
                CtorDeclaration *f = s->isCtorDeclaration();
                if (f && f->semanticRun == PASSinit)
                    f->semantic(NULL);
                return 0;
            }
        };
        for (size_t i = 0; i < members->dim; i++)
        {
            Dsymbol *s = (*members)[i];
            s->apply(&SearchCtor::fp, NULL);
        }
    }
}

/***************************************
 * Fit elements[] to the corresponding type of field[].
 * Input:
 *      loc
 *      sc
 *      elements    The explicit arguments that given to construct object.
 *      stype       The constructed object type.
 * Returns false if any errors occur.
 * Otherwise, returns true and elements[] are rewritten for the output.
 */
bool StructDeclaration::fit(Loc loc, Scope *sc, Expressions *elements, Type *stype)
{
    if (!elements)
        return true;

    size_t nfields = fields.dim - isNested();
    size_t offset = 0;
    for (size_t i = 0; i < elements->dim; i++)
    {
        Expression *e = (*elements)[i];
        if (!e)
            continue;

        e = resolveProperties(sc, e);
        if (i >= nfields)
        {
            if (i == fields.dim - 1 && isNested() && e->op == TOKnull)
            {
                // CTFE sometimes creates null as hidden pointer; we'll allow this.
                continue;
            }
            ::error(loc, "more initializers than fields (%d) of %s", nfields, toChars());
            return false;
        }
        VarDeclaration *v = fields[i];
        if (v->offset < offset)
        {
            ::error(loc, "overlapping initialization for %s", v->toChars());
            return false;
        }
        offset = (unsigned)(v->offset + v->type->size());

        Type *t = v->type;
        if (stype)
            t = t->addMod(stype->mod);
        Type *origType = t;
        Type *tb = t->toBasetype();

        /* Look for case of initializing a static array with a too-short
         * string literal, such as:
         *  char[5] foo = "abc";
         * Allow this by doing an explicit cast, which will lengthen the string
         * literal.
         */
        if (e->op == TOKstring && tb->ty == Tsarray)
        {
            StringExp *se = (StringExp *)e;
            Type *typeb = se->type->toBasetype();
            TY tynto = tb->nextOf()->ty;
            if (!se->committed &&
                (typeb->ty == Tarray || typeb->ty == Tsarray) &&
                (tynto == Tchar || tynto == Twchar || tynto == Tdchar) &&
                se->numberOfCodeUnits(tynto) < ((TypeSArray *)tb)->dim->toInteger())
            {
                e = se->castTo(sc, t);
                goto L1;
            }
        }

        while (!e->implicitConvTo(t) && tb->ty == Tsarray)
        {
            /* Static array initialization, as in:
             *  T[3][5] = e;
             */
            t = tb->nextOf();
            tb = t->toBasetype();
        }
        if (!e->implicitConvTo(t))
            t = origType;  // restore type for better diagnostic

        e = e->implicitCastTo(sc, t);
    L1:
        if (e->op == TOKerror)
            return false;

        (*elements)[i] = doCopyOrMove(sc, e);
    }
    return true;
}

/***************************************
 * Return true if struct is POD (Plain Old Data).
 * This is defined as:
 *      not nested
 *      no postblits, destructors, or assignment operators
 *      no 'ref' fields or fields that are themselves non-POD
 * The idea being these are compatible with C structs.
 */
bool StructDeclaration::isPOD()
{
    // If we've already determined whether this struct is POD.
    if (ispod != ISPODfwd)
        return (ispod == ISPODyes);

    ispod = ISPODyes;

    if (enclosing || postblit || dtor)
        ispod = ISPODno;

    // Recursively check all fields are POD.
    for (size_t i = 0; i < fields.dim; i++)
    {
        VarDeclaration *v = fields[i];
        if (v->storage_class & STCref)
        {
            ispod = ISPODno;
            break;
        }

        Type *tv = v->type->baseElemOf();
        if (tv->ty == Tstruct)
        {
            TypeStruct *ts = (TypeStruct *)tv;
            StructDeclaration *sd = ts->sym;
            if (!sd->isPOD())
            {
                ispod = ISPODno;
                break;
            }
        }
    }

    return (ispod == ISPODyes);
}

const char *StructDeclaration::kind()
{
    return "struct";
}

/********************************* UnionDeclaration ****************************/

UnionDeclaration::UnionDeclaration(Loc loc, Identifier *id)
    : StructDeclaration(loc, id)
{
}

Dsymbol *UnionDeclaration::syntaxCopy(Dsymbol *s)
{
    assert(!s);
    UnionDeclaration *ud = new UnionDeclaration(loc, ident);
    return StructDeclaration::syntaxCopy(ud);
}

const char *UnionDeclaration::kind()
{
    return "union";
}
