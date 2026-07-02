# Analyse de compatibilite — Ricoh Aficio SP C240SF

## Conclusion

Le flux DDST doit etre valide par le micrologiciel, pas seulement par le
parseur local. Le blocage de protocole confirme et corrige est le champ
`GDIJ+0x0A` : c'est le nombre de copies, pas la hauteur de la page.

La correction qui forcait le BIH JBIG a `0x48` n'est pas retenue. Le pilote
macOS de reference appelle `ImxJBIGEncoderInit` avec le mode `1`; ce chemin
produit `0x40` (`JBG_LRLTWO`). L'option `0x48` est reservee a l'autre branche
de l'encodeur. Le filtre Linux emet donc `order=0x03`, `options=0x40`.

## Corrections appliquees

| Zone | Probleme | Correction |
|---|---|---|
| `GDIJ+0x0A` | La hauteur raster etait envoyee comme nombre de copies. Une page de 240 lignes annoncait 240 copies. | Ecriture de `cups_page_header2_t.NumCopies` (ou 1 si nul). |
| JBIG BIH | `0x48` diverge du chemin `djz_Compress` du pilote macOS fourni. | `0x40` conserve dans `jbg_enc_options`. |
| Analyse du flux | Le parseur affichait ce champ comme `bitHeight`. | Il affiche maintenant `copies`. |
| Detection USB | Le helper udev abandonnait apres 5 s, alors que CUPS peut enumerer plus tard et udev ne relance pas necessairement le meme evenement. | Attente portee a 30 s et message systeme explicite. |

## Ce qui peut encore empecher une impression

1. **File CUPS non creee ou mauvaise URI.** Le filtre ne sera jamais appele.
   Verifier `lpstat -v -p RicohSPC240SF` et `lpinfo -v | grep -i ricoh`.
   Pour USB, preferer `sudo ./install-usb.sh`; pour le reseau, fournir une URI
   explicite, par exemple `sudo ./install.sh socket://IP:9100`.
2. **Filtre non lance ou raster non pris en charge.** Le journal doit contenir
   soit `rastertoddst`, soit un message `unsupported color space`. Activer le
   log CUPS avant le test : `sudo cupsctl --debug-logging`, puis consulter
   `sudo tail -f /var/log/cups/error_log`.
3. **Compatibilite firmware restante.** Le flux synthetique est structurellement
   valide, mais seul un test sur la machine physique confirme l'acceptation du
   JBIG et des en-tetes DDST. Une capture d'un travail Windows/macOS reussi est
   la reference definitive pour comparer les octets restants.
4. **Qualite couleur.** Le Bayer 8x8 est une approximation des ecrans
   proprietaires Windows. Cela peut degrader le rendu, mais n'est pas une
   cause etablie de rejet du travail par l'imprimante.

## Verification sans imprimante

```sh
make clean all
python3 make_test_raster.py > /tmp/spc240sf.raster
./rastertoddst 1 user test 1 'PageSize=A4 ColorModel=RGB' \
    /tmp/spc240sf.raster > /tmp/spc240sf.ddst
python3 parse_ddst.py /tmp/spc240sf.ddst
```

Verifier dans la sortie : `GDIJ ... copies=1`, les en-tetes `GJET`, `GDIP`,
`GDIB`, puis `JIDG`. Le premier BIH JBIG doit contenir `order=0x03` et
`options=0x40`.

## Verification sur la machine cible

```sh
sudo ./install-usb.sh
lpstat -v -p RicohSPC240SF
sudo cupsctl --debug-logging
lp -d RicohSPC240SF /usr/share/cups/data/testprint
sudo tail -n 200 /var/log/cups/error_log
```

Ne pas conclure a une compatibilite firmware complete tant que ce test sur
l'imprimante physique n'a pas ete observe.
