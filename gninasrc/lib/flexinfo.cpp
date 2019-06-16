#include "flexinfo.h"
#include <boost/lexical_cast.hpp>
#include <openbabel/mol.h>
#include <openbabel/atom.h>
#include <openbabel/bond.h>
#include <boost/unordered_map.hpp>

using namespace std;
FlexInfo::FlexInfo(const std::string& flexres, double flexdist,
    const std::string& ligand, tee& l)
    : flex_dist(flexdist), log(l) {
  using namespace OpenBabel;
  using namespace std;
  //first extract comma separated list
  if (flexres.size() > 0) {
    vector<string> tokens;
    boost::split(tokens, flexres, boost::is_any_of(","));

    vector<string> chres;
    for (unsigned i = 0, n = tokens.size(); i < n; i++) {
      //each token may be either chain:resid or just resid
      string tok = tokens[i];
      boost::split(chres, tok, boost::is_any_of(":"));
      char chain = 0;
      int resid = 0;
      char icode = 0;
      if (chres.size() >= 2) {
        if (chres[0].size() != 1)
          log << "WARNING: chain specification not single character "
              << chres[0] << "\n";
        chain = chres[0][0]; //if empty will be null which is what is desired
        resid = boost::lexical_cast<int>(chres[1]);
	if (chres.size() == 3 && chres[2].size() == 1) {
	  icode = chres[2][0];
	} else {
          log << "WARNING: ignoring invalid chain:resid:icode specifier " << tok << "\n";
	}
      } else
        if (chres.size() == 1) {
          resid = boost::lexical_cast<int>(chres[0]);
        } else {
          log << "WARNING: ignoring invalid chain:resid specifier " << tok
              << "\n";
          continue;
        }

      residues.insert(tuple<char, int, char>(chain, resid, icode));
    }
  }

  if (ligand.size() > 0 && flex_dist > 0) {
    //next read ligand for distance checking
    obmol_opener opener;
    OBConversion conv;
    opener.openForInput(conv, ligand);
    conv.Read(&distligand); //first ligand only
  }

}

void FlexInfo::extractFlex(OpenBabel::OBMol& receptor, OpenBabel::OBMol& rigid,
    std::string& flexpdbqt) {
  using namespace OpenBabel;
  rigid = receptor;
  rigid.SetChainsPerceived(true); //OB bug workaround
  flexpdbqt.clear();

  std::cerr << ">>> EXTRACT FLEXERS: OLD" << std::endl;

  //identify residues close to distligand here
  Box b;
  b.add_ligand_box(distligand);
  b.expand(flex_dist);
  double flsq = flex_dist * flex_dist;

  FOR_ATOMS_OF_MOL(a, rigid){
  if(a->GetAtomicNum() == 1)
  continue; //heavy atoms only
  vector3 v = a->GetVector();
  if (b.ptIn(v.x(), v.y(), v.z()))
  {
    //in box, see if any atoms are close enough
    FOR_ATOMS_OF_MOL(b, distligand)
    {
      vector3 bv = b->GetVector();
      if (v.distSq(bv) < flsq)
      {
        //process residue
        OBResidue *residue = a->GetResidue();
        if (residue)
        {
          char ch = residue->GetChain();
          int resid = residue->GetNum();
	  char icode = residue->GetInsertionCode();
          residues.insert(std::tuple<char, int, char>(ch, resid, icode));
        }
        break;
      }
    }
  }
}

//replace any empty chains with first chain in mol
  char defaultch = ' ';
  OBResidueIterator ritr;
  OBResidue *firstres = rigid.BeginResidue(ritr);
  if (firstres) defaultch = firstres->GetChain();

  std::vector<std::tuple<char, int, char> > sortedres(residues.begin(), residues.end());
  for (unsigned i = 0, n = sortedres.size(); i < n; i++) {
    if (get<0>(sortedres[i]) == 0) get<0>(sortedres[i]) = defaultch;
  }

  sort(sortedres.begin(), sortedres.end());

  if (sortedres.size() > 0) {
    log << "Flexible residues:";
    for (unsigned i = 0, n = sortedres.size(); i < n; i++) {
      log << " " << get<0>(sortedres[i]) << ":" << get<1>(sortedres[i]);
      if(get<2>(sortedres[i]) > 0) log << ":" << get<2>(sortedres[i]);
    }
    log << "\n";
  }
  //reinsert residues now with default chain
  residues.clear();
  residues.insert(sortedres.begin(), sortedres.end());

  OBConversion conv;
  conv.SetOutFormat("PDBQT");
  conv.AddOption("s", OBConversion::OUTOPTIONS); //flexible residue
  rigid.BeginModify();
  int flexcnt = 0;
  //identify atoms that have to be in flexible component
  //this is the side chain and CA, but _not_ the C and N
  for (OBResidueIterator ritr = rigid.BeginResidues(), rend =
      rigid.EndResidues(); ritr != rend; ++ritr) {
    OBResidue *r = *ritr;
    char ch = r->GetChain();
    int resid = r->GetNum();
    char icode = r->GetInsertionCode();
    std::tuple<char, int, char> chres(ch, resid, icode);
    if (residues.count(chres)) {
      flexcnt++;
      //create a separate molecule for each flexible residue
      OBMol flex;
      std::vector<OBAtom*> flexatoms; //rigid atom ptrs that should be flexible
      boost::unordered_map<OBAtom*, int> flexmap; //map rigid atom ptrs to atom indices in flex

      //make absolutely sure that CA is the first atom
      //first bond is rigid, so take both CA and C
      for (OBAtomIterator aitr = r->BeginAtoms(), aend = r->EndAtoms();
          aitr != aend; ++aitr) {
        OBAtom *a = *aitr;
        std::string aid = r->GetAtomID(a);
        boost::trim(aid);
        if (aid == "CA" || aid == "C") {
          flexatoms.push_back(a);
          flex.AddAtom(*a);
          flexmap[a] = flex.NumAtoms(); //after addatom since indexed by
        }
      }

      for (OBAtomIterator aitr = r->BeginAtoms(), aend = r->EndAtoms();
          aitr != aend; ++aitr) {
        OBAtom *a = *aitr;
        std::string aid = r->GetAtomID(a);
        boost::trim(aid);
        if (aid != "CA" && aid != "N" && aid != "C" && aid != "O" && aid != "H"
            && aid != "HN" && //leave backbone alone other than CA which is handled above
            !a->IsNonPolarHydrogen()) {
          flexatoms.push_back(a);
          flex.AddAtom(*a);
          flexmap[a] = flex.NumAtoms(); //after addatom since indexed by
        }
      }

      //now add bonds - at some point I should add functions to openbabel to do this..
      for (unsigned i = 0, n = flexatoms.size(); i < n; i++) {
        OBAtom *a = flexatoms[i];
        FOR_BONDS_OF_ATOM(b, a){
        OBBond& bond = *b;
        //just do one direction, if first atom is a
        //and second atom is a flexatom need to add bond
        if(a == bond.GetBeginAtom() && flexmap.count(bond.GetEndAtom()))
        {
          flex.AddBond(flexmap[a],flexmap[bond.GetEndAtom()],bond.GetBondOrder(), bond.GetFlags());
        }
      }
    }

      flex.AddResidue(*r);
      OBResidue *newres = flex.GetResidue(0);
      if (newres) {
        //add all atoms with proper atom ids
        for (unsigned i = 0, n = flexatoms.size(); i < n; i++) {
          OBAtom *origa = flexatoms[i];
          OBAtom *newa = flex.GetAtom(flexmap[origa]);
          newres->RemoveAtom(origa);
          newres->AddAtom(newa);
          newa->SetResidue(newres);
          std::string aid = r->GetAtomID(origa);
          newres->SetAtomID(newa, aid);
        }
      }

      std::string flexres = conv.WriteString(&flex);

      if (newres) {
        //the pdbqt writing code breaks flex into fragments, in the process it loses all residue information
        //so we rewrite the strings..
        std::vector<std::string> tokens;
        std::string resn = newres->GetName();
        while (resn.size() < 3)
          resn += " ";

        std::string resnum = boost::lexical_cast<std::string>(newres->GetNum());
        while (resnum.size() < 4)
          resnum = " " + resnum;

        char ch = newres->GetChain();
	char icode = newres->GetInsertionCode();
        boost::split(tokens, flexres, boost::is_any_of("\n"));
        for (unsigned i = 0, n = tokens.size(); i < n; i++) {
          std::string line = tokens[i];
          if (line.size() > 25) {
            //replace UNL with resn
            for (unsigned p = 0; p < 3; p++) {
              line[17 + p] = resn[p];
            }
            //resid
            for (unsigned p = 0; p < 4; p++) {
              line[22 + p] = resnum[p];
            }
            if(icode > 0) line[26] = icode;
            line[21] = ch;
          }

          if (line.size() > 0) {
            flexpdbqt += line;
            flexpdbqt += "\n";
          }
        }
      } else {
        flexpdbqt += flexres;
      }

      //remove flexatoms from rigid
      for (unsigned i = 0, n = flexatoms.size(); i < n; i++) {
        OBAtom *a = flexatoms[i];
        rigid.DeleteAtom(a);
      }
    } //end if residue
  }
  if(flexcnt != residues.size()) {
    log << "WARNING: Only identified " << flexcnt << " of " << residues.size() << " requested flexible residues.\n";
  }
  rigid.EndModify();

}
