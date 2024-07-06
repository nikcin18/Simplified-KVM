# Pojednostavljen KVM
*Ovaj dokument sadrži opis projekta na srpskom jeziku. Postoji i fajl [README.md](https://github.com/nikcin18/Simplified-KVM/blob/main/README.md) koji sadrži opis projekta na engleskom jeziku.*

Ovaj projekat je bio predispitna obaveza na kursu "Arhitektura i organizacija računara 2" u prolećnom semstru školske godine 2023/2024. Projekat predstavlja pojednostavljen sistem za upravljanje virtuelnim mašinama za Linux operativni sistem uz pomoć KVM (*Kernel-based Virtual Machine*) alata. U sistemu je moguće istovremeno inicijalizovanje, pokretanje i upravljanje više gostiju (tj. *guest* operativnih sistema).

Kod projekta je napisan na C programskom jeziku uz korišćenje biblioteke `kvm.h` za pristup API funkcijama KVM sistema.

Gosti mogu da komuniciraju sa terminalom kao i da pristupaju fajlovima na mašini domaćinu za čitanje ili upis podataka.

## O hipervizoru
Na početku programa hipervizora se preko komandne linije učitavaju parametri podešavanje gosta, fajlovi memorije gostiju i deljeni fajlovi. Ukoliko su uneti nevalidni argumenti, hipervizor signalizira grešku i program prestaje sa radom. U suprotnom, hipervizor inicijalizuje i pokreće goste za koje u budućnosti obrađuje sve prekide koje gosti generišu. Za svakog gosta se kreira posebna nit u kojoj se obavljaju sve radnje vezane za tog gosta. **Kod hipervizora ne treba modifikovati**.

## O gostu
Izvorni kod gosta uključuje funkcije koje predstavljaju omotače funkcionalnosti gosta kao što su rad sa terminalom i fajl sistemom. Kod dat između komentara `/* INSERT CODE BELOW THIS LINE */` i `/* INSERT CODE ABOVE THIS LINE */` predstavlja željeno "*ponašanje*" gosta. Korisnici mogu da modifikuju taj deo koda kako bi obezbedili drugačije *ponašanje* gosta. **Ostatak koda gosta (što uključuje funkcije "omotače") ne treba modifikovati**. Za potrebe projekta je obezbeđen fajl koji je šablon izvornog koda gosta. Moguće je kreirati više gostiju tako što se dati fajl kopira određeni broj puta, a svaka kopija modifikuje po želji korisnika.

## O U/I sistemu
Svaki gost može da komunicira sa terminalom, koristeći funkcije `getchar` i  `putchar`. Unutar tih funkcija, gost šalje zahteve za rad sa terminalom hipervizoru preko U/I porta 0x00E9. Veličina podataka koji prosleđuju kroz port je jedan bajt.

## O fajl sistemu
Svaki gost može da pristupa podacima unutar fajlova skladištenih na mašini domaćinu. Fajl sistem je implementiran tako da podražava POSIX fajl deskriptore. Preko fajl deskriptora, moguće je čitanje ili upis podatak u otvoreni fajl. Za te potrebe rada sa fajl sistemom obezbeđene su funkcije `fopen`, `fclose`, `fread` i `fwrite`. Unutar tih funkcija, gost šalje zahteve za rad sa fajlovima hipervizoru preko U/I porta 0x0278. Veličina podataka koji prosleđuju kroz port je jedan bajt.

Unutar fajl sistema razlikuju se lokalni i deljeni fajlovi. 

Lokalni fajlovi su vidljivi samo gostu koji ih je inicijalno kreirao za upis podataka. Ako gost pokuša da otvori ne postojeći lokalni fajl za čitanje podataka, hipervizor će signalizirati grešku koju će proslediti gostu kao odgovor na zahtev za otvaranje fajla.

Deljeni fajlovi su vidljivi svim gostima. Više gostiju može istovremeno da otvori isti fajl za čitanje podataka (svakom gostu će biti dodeljen poseban fajl deskriptor). Ukoliko gost pokuša da otvori deljeni fajl za pisanje, kreiraće se novi lokalni fajl sa istim imenom, koji će u budućnosti biti dostupan gostu umesto deljenog fajla.

Kako bi hipervizor razlikovao lokalne fajlove sa istim imenom ali od različitih gostiju, svakom lokalnom fajlu će se na naziv dodati sufiks `".local?"`, gde `"?"` predstavlja ID gosta (npr. za rad sa gostom čiji je ID broj 23 će se koristiti sufiks `".local23"`). Gost nije svestan promena naziva fajla i očekuje u svom programu originalni naziv fajla.

## Pokretanje hipervizora i postavljanje parametara podešavanja gosta
Korisnik pokreće hipervizor preko terminala pomoću komande `mini_hypervisor` sa dodatnim argumentima koji predstavljaju parametre podešavanja gosta.

### Parametar 1: veličina fizičke memorije gosta
Veličina fizičke memorije gosta se definiše pomoću opcije `-m` ili `--memory` koja je praćena vrednošću parametra. **Ovo je obavezan parametar**. Postoje tri dozvoljene vrednosti:
- vrednost `2` (veličina fizičke memorije gosta je 2MB)
- vrednost `4` (veličina fizičke memorije gosta je 4MB)
- vrednost `8` (veličina fizičke memorije gosta je 8MB)

### Parametar 2: veličina stranice virtuelne memorije gosta
Veličina stranice virtuelne memorije gosta se definiše pomoću opcije `-p` ili `--page` koja je praćena vrednošću parametra. **Ovo je obavezan parametar**. Postoje dve dozvoljene vrednosti:
- vrednost `2` (veličina stranice virtuelne memorije gosta je 2MB)
- vrednost `4` (veličina stranice virtuelne memorije gosta je 4KB)

### Parametar 3: fajl memorije gosta
Fajl memorije gosta predstavlja izvorni kod gosta preveden na mašinski jezik mašine. Pri inicijalizaciji gosta se sadržaj ovog fajla kopira u alociran prostor za fizičku memoriju gosta, dok se pri pokretanju gosta izvršava njegov preveden izvorni kod. Fajlovi memorije gosta se definišu pomoću opcije `-g` ili `--guest` koja je praćena relativnom putanjom da fajla memorije gosta za svakog gosta koji korisnik želi da pokrene. **Ovo je obavezan parametar**.

### Parametar 4: deljeni fajlovi
Deljeni fajlovi se definišu pomoću opcije `-f` ili `--file` koja je praćena relativnom putanjom do svakog deljenog fajla.

## Primer pokretanja hipervizora i gostiju
Data komanda prikazuje pokretanje sistema za virtuelne mašine gde je fizička memorija gosta veličine 8MB i stranica virtuelne memorije gosta veličine 4KB. Za inicijalizaciju gostiju se koristi sledeći fajlovi: "guest1.img","guest2.img" i "guest3.img". Deljeni fajlovi u sistemu su "shared1.txt" i "shared2.cpp".

`mini_hypervisor -m 8 -p 4 -g guest1.img guest2.img guest3.img -f shared1.txt shared2.cpp`

### Dodatak: generisanje objektnog fajla hipervizora i fajla memorije gosta
Objektni fajl hipervizora se generiše pomoću sledeće komande:
`gcc -lpthread mini_hypervisor.c -o mini_hypervisor`

Fajl memorije gosta se generiše pomoću sledećih komandi:
`$(CC) -m64 -ffreestanding -fno-pic -c -o guest.o guest.c`
`ld -T guest.ld guest.o -o guest.img`

## Važne napomene
- Više gostiju može da koristi isti fajl memorije gosta za inicijalizovanje sopstvene fizičke memorije, ali svaki gost ima posebno alociran adresni prostor za svoju fizičku memoriju, kako bi se obezbedio nezavisni rad gosta.

- Svaki gost nezavisno koristi U/I operacije. Zbog toga ukoliko više gostiju zatraži operacije čitanja ili upisa na terminal, ne može se sigurno utvrditi redosled izvršavanja operacija svih gostiju.

- Ako gost otvori deljeni fajl za čitanje, a zatim prvi put otvori isti fajl za upis, fajl deskriptor dodeljen nakon prvog otvaranja fajla će postati nevalidan. To znači da pokušaj čitanja podataka iz fajla će rezultirati u povratnoj vrednosti koja predstavlja grešku.

- Hipervizor čuva informacije samo o lokalnim fajlovima koji su kreirani tokom trenutne sesije programa. Fajlovi koji su kreirani tokom neke od prethodnih sesija neće biti vidljivi hipervizoru/gostu, štaviše ukoliko se zatraži otvaranje novog lokalnog fajla sa istim imenom, sadržaj starog lokalni fajl će biti trajno izbrisan.

## Verzije projekta
### 1.0 Početna verzija
Ovo je verzija projekta koja je napravljena kao rešenje zadatka na kursu. Zbog specifičnih zahteva zadatka, čitav izvorni kod je smešten unutar fajlova "mini_hypervisor.c" i "guest.c".