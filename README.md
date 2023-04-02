### Latea Mihai-Alexandru
### Grupa 321CA

# Server web asincron

* Aplicatia reprezinta un server web asincron ce trimite fisiere clientilor
folosind protocolul HTTP;
* Fisierele sunt de 2 tipuri: statice si dinamice.

* Fiecare conexiune este caracterizata de o stare la un anumit moment in program.
Spre exemplu, la inceput, conexiunea are starea ```CONN_INIT```.\
Dupa primirea cererii de la client in format complet, conexiunea se afla in starea ```CONN_WREADY```
care arata ca mesajul este gata de trimis catre client.

* Primirea cererii de la client nu se face instant, deoarece functia de citire din socket poate sa nu citeasca\
intregul mesaj, in acest caz conexiunea ramane in starea de ```CONN_READING```, pana cand este evenimentul\
de citire este iar semnalat in ```epoll``` si se citesc si restul de bytes.

* Primul pas este trimiterea antetului ce contine codul de status, mesajul, numele protocolului\
si alte informatii precum dimensiunea mesajului ce va fi trimis, tipul acestuia, etc.

* Momentan sunt 2 coduri ce pot fi trimise: 200 OK si 404 NOT FOUND. 

### Trimitere fisier static

* Fisierele de tip static sunt trimise direct, folosind functia ```sendfile``` fara a fi nevoie de a mai folosi\
buffere suplimentare. Conexiunea se afla in starea ```CONN_SENDING``` pana cand ```sendfile``` trimite numarul\
complet de bytes la client.
* Conexiunea este incheiata apoi, eliberand memoria folosita de structura de ce reprezinta conexiunea.

### Trimitere fisiere dinamice

* Fisierele de tip dinamic au nevoie de un pas in care sunt incarcate in memorie folosind\
o lista de buffere care sunt populate cu datele efective folosind operatii asincrone. 
* Cat timp fiserul este incarcat asincron in memorie, conexiunea se afla in starea ```CONN_READING_ASYNC```\
lasand loc altor conexiuni pentru a fi completate.
* Dupa ce tot fisierul a fost incarcat in memorie, bufferele sunt trimise pe rand clientului\
folosind functia de ```send``` avand in vedere ca poate fi trimis doar o parte din buffer, astfel\
actiunea continuand cand este semnalata in epoll.

## Nota
* Atat antetul cat si bufferele pentru fisier in cazul fisierelor dinamice sunt\
trimise folosind metoda ```send_message``` care trimite pe socketul conexiunii datele care\
se afla in ```conn.write_buff```.
